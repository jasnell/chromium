// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_IPC_SERVICE_IMAGE_DECODE_ACCELERATOR_STUB_H_
#define GPU_IPC_SERVICE_IMAGE_DECODE_ACCELERATOR_STUB_H_

#include <stdint.h>

#include <memory>

#include "base/containers/queue.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "gpu/command_buffer/service/sequence_id.h"
#include "gpu/ipc/common/gpu_messages.h"
#include "gpu/ipc/service/image_decode_accelerator_worker.h"
#include "ui/gfx/geometry/size.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace IPC {
class Message;
}  // namespace IPC

namespace gpu {
class GpuChannel;
class SyncPointClientState;

// Processes incoming image decode requests from renderers: it schedules the
// decode with the appropriate hardware decode accelerator and releases sync
// tokens as decodes complete. These sync tokens must be generated on the client
// side (in ImageDecodeAcceleratorProxy) using the following information:
//
// - The command buffer namespace is GPU_IO.
// - The command buffer ID is created using the
//   CommandBufferIdFromChannelAndRoute() function using
//   GpuChannelReservedRoutes::kImageDecodeAccelerator as the route ID.
// - The release count should be incremented for each decode request.
//
// An object of this class is meant to be used in
// both the IO thread (for receiving decode requests) and the main thread (for
// processing completed decodes).
class ImageDecodeAcceleratorStub
    : public base::RefCountedThreadSafe<ImageDecodeAcceleratorStub> {
 public:
  // TODO(andrescj): right now, we only accept one worker to be used for JPEG
  // decoding. If we want to use multiple workers, we need to ensure that sync
  // tokens are released in order.
  ImageDecodeAcceleratorStub(ImageDecodeAcceleratorWorker* worker,
                             GpuChannel* channel,
                             int32_t route_id);

  // Processes a message from the renderer. Should be called on the IO thread.
  bool OnMessageReceived(const IPC::Message& msg);

  // Called on the main thread to indicate that |channel_| should no longer be
  // used.
  void Shutdown();

 private:
  friend class base::RefCountedThreadSafe<ImageDecodeAcceleratorStub>;
  ~ImageDecodeAcceleratorStub();

  void OnScheduleImageDecode(
      const GpuChannelMsg_ScheduleImageDecode_Params& params,
      uint64_t release_count);

  // Creates the service-side cache entry for a completed decode and releases
  // the decode sync token.
  void ProcessCompletedDecode(GpuChannelMsg_ScheduleImageDecode_Params params,
                              uint64_t decode_release_count);

  // The |worker_| calls this when a decode is completed. If the decode is
  // successful, |sequence_| will be enabled so that ProcessCompletedDecode() is
  // called. If the decode is not successful, we destroy the channel (see
  // OnError()).
  void OnDecodeCompleted(
      gfx::Size expected_output_size,
      std::unique_ptr<ImageDecodeAcceleratorWorker::DecodeResult> result);

  // Triggers the destruction of the channel asynchronously and makes it so that
  // we stop accepting completed decodes. On entry, |channel_| must not be
  // nullptr.
  void OnError() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // The object to which the actual decoding can be delegated.
  ImageDecodeAcceleratorWorker* worker_ = nullptr;

  base::Lock lock_;
  GpuChannel* channel_ GUARDED_BY(lock_) = nullptr;
  SequenceId sequence_ GUARDED_BY(lock_);
  scoped_refptr<SyncPointClientState> sync_point_client_state_
      GUARDED_BY(lock_);
  base::queue<std::unique_ptr<ImageDecodeAcceleratorWorker::DecodeResult>>
      pending_completed_decodes_ GUARDED_BY(lock_);
  bool destroying_channel_ GUARDED_BY(lock_) = false;
  uint64_t last_release_count_ GUARDED_BY(lock_) = 0;

  scoped_refptr<base::SingleThreadTaskRunner> main_task_runner_;
  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;

  DISALLOW_COPY_AND_ASSIGN(ImageDecodeAcceleratorStub);
};

}  // namespace gpu

#endif  // GPU_IPC_SERVICE_IMAGE_DECODE_ACCELERATOR_STUB_H_
