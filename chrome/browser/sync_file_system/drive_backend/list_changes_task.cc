// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync_file_system/drive_backend/list_changes_task.h"

#include "base/bind.h"
#include "base/format_macros.h"
#include "base/location.h"
#include "chrome/browser/drive/drive_api_util.h"
#include "chrome/browser/drive/drive_service_interface.h"
#include "chrome/browser/sync_file_system/drive_backend/drive_backend_util.h"
#include "chrome/browser/sync_file_system/drive_backend/metadata_database.h"
#include "chrome/browser/sync_file_system/drive_backend/metadata_database.pb.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_engine_context.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_task_manager.h"
#include "chrome/browser/sync_file_system/drive_backend/sync_task_token.h"
#include "chrome/browser/sync_file_system/logger.h"
#include "chrome/browser/sync_file_system/syncable_file_system_util.h"
#include "google_apis/drive/drive_api_parser.h"
#include "google_apis/drive/gdata_wapi_parser.h"

namespace sync_file_system {
namespace drive_backend {

namespace {

scoped_ptr<google_apis::ChangeResource> ConvertResourceEntryToChangeResource(
    const google_apis::ResourceEntry& entry) {
  scoped_ptr<google_apis::ChangeResource> out(new google_apis::ChangeResource);
  out->set_file_id(entry.resource_id());
  if (!entry.deleted())
    out->set_file(drive::util::ConvertResourceEntryToFileResource(entry));
  out->set_change_id(entry.changestamp());
  out->set_deleted(entry.deleted());

  return out.Pass();
}

}  // namespace

ListChangesTask::ListChangesTask(SyncEngineContext* sync_context)
    : sync_context_(sync_context),
      weak_ptr_factory_(this) {
}

ListChangesTask::~ListChangesTask() {
}

void ListChangesTask::Run(scoped_ptr<SyncTaskToken> token) {
  util::Log(logging::LOG_VERBOSE, FROM_HERE, "[Changes] Start.");

  if (!IsContextReady()) {
    util::Log(logging::LOG_VERBOSE, FROM_HERE,
              "[Changes] Failed to get required service.");
    RunSoon(FROM_HERE, base::Bind(&SyncTaskManager::NotifyTaskDone,
                                  base::Passed(&token),
                                  SYNC_STATUS_FAILED));
    return;
  }

  drive_service()->GetChangeList(
      metadata_database()->GetLargestFetchedChangeID() + 1,
      base::Bind(&ListChangesTask::DidListChanges,
                 weak_ptr_factory_.GetWeakPtr(), base::Passed(&token)));
}

void ListChangesTask::DidListChanges(
    scoped_ptr<SyncTaskToken> token,
    google_apis::GDataErrorCode error,
    scoped_ptr<google_apis::ResourceList> resource_list) {
  SyncStatusCode status = GDataErrorCodeToSyncStatusCode(error);
  if (status != SYNC_STATUS_OK) {
    util::Log(logging::LOG_VERBOSE, FROM_HERE,
              "[Changes] Failed to fetch change list.");
    SyncTaskManager::NotifyTaskDone(
        token.Pass(), SYNC_STATUS_NETWORK_ERROR);
    return;
  }

  if (!resource_list) {
    NOTREACHED();
    util::Log(logging::LOG_VERBOSE, FROM_HERE,
              "[Changes] Got invalid change list.");
    SyncTaskManager::NotifyTaskDone(
        token.Pass(), SYNC_STATUS_FAILED);
    return;
  }

  change_list_.reserve(change_list_.size() + resource_list->entries().size());
  for (size_t i = 0; i < resource_list->entries().size(); ++i) {
    change_list_.push_back(ConvertResourceEntryToChangeResource(
        *resource_list->entries()[i]).release());
  }

  // TODO(tzik): http://crbug.com/310964
  // This may take long time to run in single task.  Run this as a background
  // task.
  GURL next_feed;
  if (resource_list->GetNextFeedURL(&next_feed)) {
    drive_service()->GetRemainingChangeList(
        next_feed,
        base::Bind(
            &ListChangesTask::DidListChanges,
            weak_ptr_factory_.GetWeakPtr(),
            base::Passed(&token)));
    return;
  }

  if (change_list_.empty()) {
    util::Log(logging::LOG_VERBOSE, FROM_HERE, "[Changes] Got no change.");
    SyncTaskManager::NotifyTaskDone(
        token.Pass(), SYNC_STATUS_NO_CHANGE_TO_SYNC);
    return;
  }

  util::Log(logging::LOG_VERBOSE, FROM_HERE,
            "[Changes] Got %" PRIuS " changes, updating MetadataDatabase.",
            change_list_.size());
  metadata_database()->UpdateByChangeList(
      resource_list->largest_changestamp(),
      change_list_.Pass(),
      base::Bind(&SyncTaskManager::NotifyTaskDone, base::Passed(&token)));
}

bool ListChangesTask::IsContextReady() {
  return sync_context_->GetMetadataDatabase() &&
      sync_context_->GetDriveService();
}

MetadataDatabase* ListChangesTask::metadata_database() {
  return sync_context_->GetMetadataDatabase();
}

drive::DriveServiceInterface* ListChangesTask::drive_service() {
  set_used_network(true);
  return sync_context_->GetDriveService();
}

}  // namespace drive_backend
}  // namespace sync_file_system
