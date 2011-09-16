// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_GPU_GPU_COMMAND_BUFFER_STUB_H_
#define CONTENT_COMMON_GPU_GPU_COMMAND_BUFFER_STUB_H_
#pragma once

#if defined(ENABLE_GPU)

#include <string>
#include <vector>

#include "base/id_map.h"
#include "base/memory/weak_ptr.h"
#include "base/task.h"
#include "content/common/gpu/media/gpu_video_decode_accelerator.h"
#include "gpu/command_buffer/service/command_buffer_service.h"
#include "gpu/command_buffer/service/context_group.h"
#include "gpu/command_buffer/service/gpu_scheduler.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_message.h"
#include "ui/gfx/gl/gl_context.h"
#include "ui/gfx/gl/gl_surface.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/gfx/size.h"
#include "ui/gfx/surface/transport_dib.h"

#if defined(OS_MACOSX)
#include "ui/gfx/surface/accelerated_surface_mac.h"
#endif

class GpuChannel;
class GpuWatchdog;

class GpuCommandBufferStub
    : public IPC::Channel::Listener,
      public IPC::Message::Sender,
      public base::SupportsWeakPtr<GpuCommandBufferStub> {
 public:
  GpuCommandBufferStub(
      GpuChannel* channel,
      GpuCommandBufferStub* share_group,
      gfx::PluginWindowHandle handle,
      const gfx::Size& size,
      const gpu::gles2::DisallowedExtensions& disallowed_extensions,
      const std::string& allowed_extensions,
      const std::vector<int32>& attribs,
      int32 route_id,
      int32 renderer_id,
      int32 render_view_id,
      GpuWatchdog* watchdog,
      bool software);

  virtual ~GpuCommandBufferStub();

  // IPC::Channel::Listener implementation:
  virtual bool OnMessageReceived(const IPC::Message& message);

  // IPC::Message::Sender implementation:
  virtual bool Send(IPC::Message* msg);

  // Whether this command buffer can currently handle IPC messages.
  bool IsScheduled();

  gpu::gles2::GLES2Decoder* decoder() const { return decoder_.get(); }
  gpu::GpuScheduler* scheduler() const { return scheduler_.get(); }

  // Identifies the renderer process.
  int32 renderer_id() const { return renderer_id_; }

  // Identifies a particular renderer belonging to the same renderer process.
  int32 render_view_id() const { return render_view_id_; }

  // Identifies the various GpuCommandBufferStubs in the GPU process belonging
  // to the same renderer process.
  int32 route_id() const { return route_id_; }

  void ViewResized();

#if defined(OS_MACOSX)
  // Called only by the GpuChannel.
  void AcceleratedSurfaceBuffersSwapped(uint64 swap_buffers_count);

  // Needed only on Mac OS X, which does not render into an on-screen
  // window and therefore requires the backing store to be resized
  // manually. Returns an opaque identifier for the new backing store.
  // There are two versions of this method: one for use with the IOSurface
  // available in Mac OS X 10.6; and, one for use with the
  // TransportDIB-based version used on Mac OS X 10.5.
  virtual TransportDIB::Handle SetWindowSizeForTransportDIB(
      const gfx::Size& size);
  virtual void SetTransportDIBAllocAndFree(
      Callback2<size_t, TransportDIB::Handle*>::Type* allocator,
      Callback1<TransportDIB::Id>::Type* deallocator);
#endif

 private:
  void Destroy();

  // Cleans up and sends reply if OnInitialize failed.
  void OnInitializeFailed(IPC::Message* reply_message);

  // Message handlers:
  void OnInitialize(base::SharedMemoryHandle ring_buffer,
                    int32 size,
                    IPC::Message* reply_message);
  void OnSetParent(int32 parent_route_id,
                   uint32 parent_texture_id,
                   IPC::Message* reply_message);
  void OnGetState(IPC::Message* reply_message);
  void OnFlush(int32 put_offset,
               int32 last_known_get,
               uint32 flush_count,
               IPC::Message* reply_message);
  void OnAsyncFlush(int32 put_offset, uint32 flush_count);
  void OnEcho(const IPC::Message& message);
  void OnRescheduled();
  void OnCreateTransferBuffer(int32 size,
                              int32 id_request,
                              IPC::Message* reply_message);
  void OnRegisterTransferBuffer(base::SharedMemoryHandle transfer_buffer,
                                size_t size,
                                int32 id_request,
                                IPC::Message* reply_message);
  void OnDestroyTransferBuffer(int32 id, IPC::Message* reply_message);
  void OnGetTransferBuffer(int32 id, IPC::Message* reply_message);

  void OnCreateVideoDecoder(
      media::VideoDecodeAccelerator::Profile profile,
      IPC::Message* reply_message);
  void OnDestroyVideoDecoder(int32 decoder_route_id);

#if defined(OS_MACOSX)
  void OnSwapBuffers();

  // Returns the id of the current surface that is being rendered to
  // (or 0 if no such surface has been created).
  uint64 GetSurfaceId();
#endif

  void OnCommandProcessed();
  void OnParseError();

  void OnResize(gfx::Size size);
  void ReportState();

  // The lifetime of objects of this class is managed by a GpuChannel. The
  // GpuChannels destroy all the GpuCommandBufferStubs that they own when they
  // are destroyed. So a raw pointer is safe.
  GpuChannel* channel_;

  // The group of contexts that share namespaces with this context.
  scoped_refptr<gpu::gles2::ContextGroup> context_group_;

  gfx::PluginWindowHandle handle_;
  gfx::Size initial_size_;
  gpu::gles2::DisallowedExtensions disallowed_extensions_;
  std::string allowed_extensions_;
  std::vector<int32> requested_attribs_;
  int32 route_id_;
  bool software_;
  uint32 last_flush_count_;

  // The following two fields are used on Mac OS X to identify the window
  // for the rendering results on the browser side.
  int32 renderer_id_;
  int32 render_view_id_;

  scoped_ptr<gpu::CommandBufferService> command_buffer_;
  scoped_ptr<gpu::gles2::GLES2Decoder> decoder_;
  scoped_ptr<gpu::GpuScheduler> scheduler_;
  scoped_refptr<gfx::GLContext> context_;
  scoped_refptr<gfx::GLSurface> surface_;

  // SetParent may be called before Initialize, in which case we need to keep
  // around the parent stub, so that Initialize can set the parent correctly.
  base::WeakPtr<GpuCommandBufferStub> parent_stub_for_initialization_;
  uint32 parent_texture_for_initialization_;

  GpuWatchdog* watchdog_;

  // Zero or more video decoders owned by this stub, keyed by their
  // decoder_route_id.
  IDMap<GpuVideoDecodeAccelerator, IDMapOwnPointer> video_decoders_;

#if defined(OS_MACOSX)
  // To prevent the GPU process from overloading the browser process,
  // we need to track the number of swap buffers calls issued and
  // acknowledged per on-screen context, and keep the GPU from getting
  // too far ahead of the browser. Note that this
  // is also predicated on a flow control mechanism between the
  // renderer and GPU processes.
  uint64 swap_buffers_count_;
  uint64 acknowledged_swap_buffers_count_;
  scoped_ptr<AcceleratedSurface> accelerated_surface_;
  scoped_ptr<Callback0::Type> wrapped_swap_buffers_callback_;
#endif

  ScopedRunnableMethodFactory<GpuCommandBufferStub> task_factory_;

  DISALLOW_COPY_AND_ASSIGN(GpuCommandBufferStub);
};

#endif  // defined(ENABLE_GPU)

#endif  // CONTENT_COMMON_GPU_GPU_COMMAND_BUFFER_STUB_H_
