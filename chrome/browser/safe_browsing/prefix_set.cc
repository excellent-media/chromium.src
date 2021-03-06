// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/prefix_set.h"

#include <algorithm>
#include <math.h>

#include "base/file_util.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/md5.h"
#include "base/metrics/histogram.h"

namespace {

// |kMagic| should be reasonably unique, and not match itself across
// endianness changes.  I generated this value with:
// md5 -qs chrome/browser/safe_browsing/prefix_set.cc | colrm 9
static uint32 kMagic = 0x864088dd;

// TODO(shess): Update v2 history info once landed.

// Version history:
// Version 1: b6cb7cfe/r74487 by shess@chromium.org on 2011-02-10
// version 2: ????????/r????? by shess@chromium.org on 2014-02-24

// Version 2 layout is identical to version 1.  The sort order of |index_|
// changed from |int32| to |uint32| to match the change of |SBPrefix|.
static uint32 kVersion = 0x2;

typedef struct {
  uint32 magic;
  uint32 version;
  uint32 index_size;
  uint32 deltas_size;
} FileHeader;

// Common std::vector<> implementations add capacity by multiplying from the
// current size (usually either by 2 or 1.5) to satisfy push_back() running in
// amortized constant time.  This is not necessary for insert() at end(), but
// AFAICT it seems true for some implementations.  SBPrefix values should
// uniformly cover the 32-bit space, so the final size can be estimated given a
// subset of the input.
//
// |kEstimateThreshold| is when estimates start converging.  Results are strong
// starting around 1<<27.  1<<30 is chosen to prevent the degenerate case of
// resizing capacity from >50% to 100%.
//
// TODO(shess): I'm sure there is math in the world to describe good settings
// for estimating the size of a uniformly-distributed set of integers from a
// sorted subset.  I do not have such math in me, so I assumed that my current
// organic database of prefixes was scale-free, and wrote a script to see how
// often given slop values would always suffice for given strides.  At 1<<30,
// .5% slop was sufficient to cover all cases (though the code below uses 1%).
//
// TODO(shess): A smaller threshold uses less transient space in reallocation.
// 1<<30 uses between 125% and 150%, 1<<29 between 112% and 125%, etc.  The cost
// is that a smaller threshold needs more slop (locked down for the long term).
// 1<<29 worked well with 1%, 1<<27 worked well with 2%.
const SBPrefix kEstimateThreshold = 1 << 30;
size_t EstimateFinalCount(SBPrefix current_prefix, size_t current_count) {
  // estimated_count / current_count == estimated_max / current_prefix
  // For large input sets, estimated_max of 2^32 is close enough.
  const size_t estimated_prefix_count = static_cast<size_t>(
      (static_cast<uint64>(current_count) << 32) / current_prefix);

  // The estimate has an error bar, if the final total is below the estimate, no
  // harm, but if it is above a capacity resize will happen at nearly 100%.  Add
  // some slop to make sure all cases are covered.
  return estimated_prefix_count + estimated_prefix_count / 100;
}

}  // namespace

namespace safe_browsing {

// For |std::upper_bound()| to find a prefix w/in a vector of pairs.
// static
bool PrefixSet::PrefixLess(const IndexPair& a, const IndexPair& b) {
  return a.first < b.first;
}

PrefixSet::PrefixSet() {
}

PrefixSet::PrefixSet(IndexVector* index, std::vector<uint16>* deltas) {
  DCHECK(index && deltas);
  index_.swap(*index);
  deltas_.swap(*deltas);
}

PrefixSet::~PrefixSet() {}

bool PrefixSet::Exists(SBPrefix prefix) const {
  if (index_.empty())
    return false;

  // Find the first position after |prefix| in |index_|.
  IndexVector::const_iterator iter =
      std::upper_bound(index_.begin(), index_.end(),
                       IndexPair(prefix, 0), PrefixLess);

  // |prefix| comes before anything that's in the set.
  if (iter == index_.begin())
    return false;

  // Capture the upper bound of our target entry's deltas.
  const size_t bound = (iter == index_.end() ? deltas_.size() : iter->second);

  // Back up to the entry our target is in.
  --iter;

  // All prefixes in |index_| are in the set.
  SBPrefix current = iter->first;
  if (current == prefix)
    return true;

  // Scan forward accumulating deltas while a match is possible.
  for (size_t di = iter->second; di < bound && current < prefix; ++di) {
    current += deltas_[di];
  }

  return current == prefix;
}

void PrefixSet::GetPrefixes(std::vector<SBPrefix>* prefixes) const {
  prefixes->reserve(index_.size() + deltas_.size());

  for (size_t ii = 0; ii < index_.size(); ++ii) {
    // The deltas for this |index_| entry run to the next index entry,
    // or the end of the deltas.
    const size_t deltas_end =
        (ii + 1 < index_.size()) ? index_[ii + 1].second : deltas_.size();

    SBPrefix current = index_[ii].first;
    prefixes->push_back(current);
    for (size_t di = index_[ii].second; di < deltas_end; ++di) {
      current += deltas_[di];
      prefixes->push_back(current);
    }
  }
}

// static
scoped_ptr<PrefixSet> PrefixSet::LoadFile(const base::FilePath& filter_name) {
  int64 size_64;
  if (!base::GetFileSize(filter_name, &size_64))
    return scoped_ptr<PrefixSet>();
  using base::MD5Digest;
  if (size_64 < static_cast<int64>(sizeof(FileHeader) + sizeof(MD5Digest)))
    return scoped_ptr<PrefixSet>();

  base::ScopedFILE file(base::OpenFile(filter_name, "rb"));
  if (!file.get())
    return scoped_ptr<PrefixSet>();

  FileHeader header;
  size_t read = fread(&header, sizeof(header), 1, file.get());
  if (read != 1)
    return scoped_ptr<PrefixSet>();

  // TODO(shess): Version 1 and 2 use the same file structure, with version 1
  // data using a signed sort.  For M-35, the data is re-sorted before return.
  // After M-35, just drop v1 support. <http://crbug.com/346405>
  if (header.magic != kMagic ||
      (header.version != kVersion && header.version != 1)) {
    return scoped_ptr<PrefixSet>();
  }

  IndexVector index;
  const size_t index_bytes = sizeof(index[0]) * header.index_size;

  std::vector<uint16> deltas;
  const size_t deltas_bytes = sizeof(deltas[0]) * header.deltas_size;

  // Check for bogus sizes before allocating any space.
  const size_t expected_bytes =
      sizeof(header) + index_bytes + deltas_bytes + sizeof(MD5Digest);
  if (static_cast<int64>(expected_bytes) != size_64)
    return scoped_ptr<PrefixSet>();

  // The file looks valid, start building the digest.
  base::MD5Context context;
  base::MD5Init(&context);
  base::MD5Update(&context, base::StringPiece(reinterpret_cast<char*>(&header),
                                              sizeof(header)));

  // Read the index vector.  Herb Sutter indicates that vectors are
  // guaranteed to be contiuguous, so reading to where element 0 lives
  // is valid.
  if (header.index_size) {
    index.resize(header.index_size);
    read = fread(&(index[0]), sizeof(index[0]), index.size(), file.get());
    if (read != index.size())
      return scoped_ptr<PrefixSet>();
    base::MD5Update(&context,
                    base::StringPiece(reinterpret_cast<char*>(&(index[0])),
                                      index_bytes));
  }

  // Read vector of deltas.
  if (header.deltas_size) {
    deltas.resize(header.deltas_size);
    read = fread(&(deltas[0]), sizeof(deltas[0]), deltas.size(), file.get());
    if (read != deltas.size())
      return scoped_ptr<PrefixSet>();
    base::MD5Update(&context,
                    base::StringPiece(reinterpret_cast<char*>(&(deltas[0])),
                                      deltas_bytes));
  }

  base::MD5Digest calculated_digest;
  base::MD5Final(&calculated_digest, &context);

  base::MD5Digest file_digest;
  read = fread(&file_digest, sizeof(file_digest), 1, file.get());
  if (read != 1)
    return scoped_ptr<PrefixSet>();

  if (0 != memcmp(&file_digest, &calculated_digest, sizeof(file_digest)))
    return scoped_ptr<PrefixSet>();

  // For version 1, fetch the prefixes and re-sort.
  if (header.version == 1) {
    std::vector<SBPrefix> prefixes;
    PrefixSet(&index, &deltas).GetPrefixes(&prefixes);
    std::sort(prefixes.begin(), prefixes.end());
    return PrefixSetBuilder(prefixes).GetPrefixSet().Pass();
  }

  // Steals contents of |index| and |deltas| via swap().
  return scoped_ptr<PrefixSet>(new PrefixSet(&index, &deltas));
}

bool PrefixSet::WriteFile(const base::FilePath& filter_name) const {
  FileHeader header;
  header.magic = kMagic;
  header.version = kVersion;
  header.index_size = static_cast<uint32>(index_.size());
  header.deltas_size = static_cast<uint32>(deltas_.size());

  // Sanity check that the 32-bit values never mess things up.
  if (static_cast<size_t>(header.index_size) != index_.size() ||
      static_cast<size_t>(header.deltas_size) != deltas_.size()) {
    NOTREACHED();
    return false;
  }

  base::ScopedFILE file(base::OpenFile(filter_name, "wb"));
  if (!file.get())
    return false;

  base::MD5Context context;
  base::MD5Init(&context);

  // TODO(shess): The I/O code in safe_browsing_store_file.cc would
  // sure be useful about now.
  size_t written = fwrite(&header, sizeof(header), 1, file.get());
  if (written != 1)
    return false;
  base::MD5Update(&context, base::StringPiece(reinterpret_cast<char*>(&header),
                                              sizeof(header)));

  // As for reads, the standard guarantees the ability to access the
  // contents of the vector by a pointer to an element.
  if (index_.size()) {
    const size_t index_bytes = sizeof(index_[0]) * index_.size();
    written = fwrite(&(index_[0]), sizeof(index_[0]), index_.size(),
                     file.get());
    if (written != index_.size())
      return false;
    base::MD5Update(&context,
                    base::StringPiece(
                        reinterpret_cast<const char*>(&(index_[0])),
                        index_bytes));
  }

  if (deltas_.size()) {
    const size_t deltas_bytes = sizeof(deltas_[0]) * deltas_.size();
    written = fwrite(&(deltas_[0]), sizeof(deltas_[0]), deltas_.size(),
                     file.get());
    if (written != deltas_.size())
      return false;
    base::MD5Update(&context,
                    base::StringPiece(
                        reinterpret_cast<const char*>(&(deltas_[0])),
                        deltas_bytes));
  }

  base::MD5Digest digest;
  base::MD5Final(&digest, &context);
  written = fwrite(&digest, sizeof(digest), 1, file.get());
  if (written != 1)
    return false;

  // TODO(shess): Can this code check that the close was successful?
  file.reset();

  return true;
}

void PrefixSet::AddRun(SBPrefix index_prefix,
                       const uint16* run_begin, const uint16* run_end) {
  // Preempt organic capacity decisions for |delta_| once strong estimates can
  // be made.
  if (index_prefix > kEstimateThreshold &&
      deltas_.capacity() < deltas_.size() + (run_end - run_begin)) {
    deltas_.reserve(EstimateFinalCount(index_prefix, deltas_.size()));
  }

  index_.push_back(std::make_pair(index_prefix, deltas_.size()));
  deltas_.insert(deltas_.end(), run_begin, run_end);
}

PrefixSetBuilder::PrefixSetBuilder()
    : prefix_set_(new PrefixSet()) {
}

PrefixSetBuilder::PrefixSetBuilder(const std::vector<SBPrefix>& prefixes)
    : prefix_set_(new PrefixSet()) {
  for (size_t i = 0; i < prefixes.size(); ++i) {
    AddPrefix(prefixes[i]);
  }
}

PrefixSetBuilder::~PrefixSetBuilder() {
}

scoped_ptr<PrefixSet> PrefixSetBuilder::GetPrefixSet() {
  DCHECK(prefix_set_.get());

  // Flush runs until buffered data is gone.
  while (!buffer_.empty()) {
    EmitRun();
  }

  // Precisely size |index_| for read-only.  It's 50k-60k, so minor savings, but
  // they're almost free.
  PrefixSet::IndexVector(prefix_set_->index_).swap(prefix_set_->index_);

  return prefix_set_.Pass();
}


void PrefixSetBuilder::EmitRun() {
  DCHECK(prefix_set_.get());

  SBPrefix prev_prefix = buffer_[0];
  uint16 run[PrefixSet::kMaxRun];
  size_t run_pos = 0;

  size_t i;
  for (i = 1; i < buffer_.size() && run_pos < PrefixSet::kMaxRun; ++i) {
    // Calculate the delta.  |unsigned| is mandatory, because the
    // sorted_prefixes could be more than INT_MAX apart.
    DCHECK_GT(buffer_[i], prev_prefix);
    const unsigned delta = buffer_[i] - prev_prefix;
    const uint16 delta16 = static_cast<uint16>(delta);

    // Break the run if the delta doesn't fit.
    if (delta != static_cast<unsigned>(delta16))
      break;

    // Continue the run of deltas.
    run[run_pos++] = delta16;
    DCHECK_EQ(static_cast<unsigned>(run[run_pos - 1]), delta);

    prev_prefix = buffer_[i];
  }
  prefix_set_->AddRun(buffer_[0], run, run + run_pos);
  buffer_.erase(buffer_.begin(), buffer_.begin() + i);
}

void PrefixSetBuilder::AddPrefix(SBPrefix prefix) {
  DCHECK(prefix_set_.get());

  if (buffer_.empty()) {
    DCHECK(prefix_set_->index_.empty());
    DCHECK(prefix_set_->deltas_.empty());
  } else {
    // Drop duplicates.
    if (buffer_.back() == prefix)
      return;

    DCHECK_LT(buffer_.back(), prefix);
  }
  buffer_.push_back(prefix);

  // Flush buffer when a run can be constructed.  +1 for the index item, and +1
  // to leave at least one item in the buffer for dropping duplicates.
  if (buffer_.size() > PrefixSet::kMaxRun + 2)
    EmitRun();
}

}  // namespace safe_browsing
