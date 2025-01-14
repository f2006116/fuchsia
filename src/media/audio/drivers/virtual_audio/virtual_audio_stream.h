// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_
#define GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_

#include <audio-proto/audio-proto.h>
#include <dispatcher-pool/dispatcher-timer.h>
#include <dispatcher-pool/dispatcher-wakeup-event.h>
#include <fbl/ref_ptr.h>
#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/simple-audio-stream/simple-audio-stream.h>

#include <deque>

namespace virtual_audio {

class VirtualAudioDeviceImpl;

class VirtualAudioStream : public audio::SimpleAudioStream {
 public:
  void EnqueuePlugChange(bool plugged) __TA_EXCLUDES(wakeup_queue_lock_);
  void EnqueueGainRequest(
      fuchsia::virtualaudio::Device::GetGainCallback gain_callback)
      __TA_EXCLUDES(wakeup_queue_lock_);
  void EnqueueFormatRequest(
      fuchsia::virtualaudio::Device::GetFormatCallback format_callback)
      __TA_EXCLUDES(wakeup_queue_lock_);
  void EnqueueBufferRequest(
      fuchsia::virtualaudio::Device::GetBufferCallback buffer_callback)
      __TA_EXCLUDES(wakeup_queue_lock_);
  void EnqueuePositionRequest(
      fuchsia::virtualaudio::Device::GetPositionCallback position_callback)
      __TA_EXCLUDES(wakeup_queue_lock_);
  void EnqueueNotificationOverride(uint32_t notifications_per_ring)
      __TA_EXCLUDES(wakeup_queue_lock_);

  static fbl::RefPtr<VirtualAudioStream> CreateStream(
      VirtualAudioDeviceImpl* owner, zx_device_t* devnode, bool is_input);

  // Only set by DeviceImpl -- on dtor, Disable or Remove
  bool shutdown_by_parent_ = false;

 protected:
  friend class audio::SimpleAudioStream;
  friend class fbl::RefPtr<VirtualAudioStream>;

  VirtualAudioStream(VirtualAudioDeviceImpl* parent, zx_device_t* dev_node,
                     bool is_input)
      : audio::SimpleAudioStream(dev_node, is_input), parent_(parent) {}
  ~VirtualAudioStream() override;

  zx_status_t Init() __TA_REQUIRES(domain_->token()) override;
  zx_status_t InitPost() override;

  zx_status_t ChangeFormat(const audio::audio_proto::StreamSetFmtReq& req)
      __TA_REQUIRES(domain_->token()) override;
  zx_status_t SetGain(const audio::audio_proto::SetGainReq& req)
      __TA_REQUIRES(domain_->token()) override;

  zx_status_t GetBuffer(const audio::audio_proto::RingBufGetBufferReq& req,
                        uint32_t* out_num_rb_frames, zx::vmo* out_buffer)
      __TA_REQUIRES(domain_->token()) override;

  zx_status_t Start(uint64_t* out_start_time)
      __TA_REQUIRES(domain_->token()) override;
  zx_status_t Stop() __TA_REQUIRES(domain_->token()) override;

  void ShutdownHook() __TA_REQUIRES(domain_->token()) override;
  // RingBufferShutdown() is unneeded: no hardware shutdown tasks needed...

  zx_status_t ProcessRingNotification() __TA_REQUIRES(domain_->token());
  zx_status_t ProcessAltRingNotification() __TA_REQUIRES(domain_->token());

  enum class PlugType { Plug, Unplug };

  void HandlePlugChanges() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);
  void HandlePlugChange(PlugType plug_change) __TA_REQUIRES(domain_->token());

  void HandleGainRequests() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);
  void HandleFormatRequests() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);
  void HandleBufferRequests() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);
  void HandlePositionRequests() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);
  void HandleSetNotifications() __TA_REQUIRES(domain_->token())
      __TA_EXCLUDES(wakeup_queue_lock_);

  // Accessed in GetBuffer, defended by token.
  fzl::VmoMapper ring_buffer_mapper_ __TA_GUARDED(domain_->token());
  zx::vmo ring_buffer_vmo_ __TA_GUARDED(domain_->token());
  uint32_t num_ring_buffer_frames_ __TA_GUARDED(domain_->token()) = 0;

  uint32_t max_buffer_frames_ __TA_GUARDED(domain_->token());
  uint32_t min_buffer_frames_ __TA_GUARDED(domain_->token());
  uint32_t modulo_buffer_frames_ __TA_GUARDED(domain_->token());

  fbl::RefPtr<dispatcher::Timer> notify_timer_;
  fbl::RefPtr<dispatcher::Timer> alt_notify_timer_;

  zx::time start_time_ __TA_GUARDED(domain_->token());
  zx::duration notification_period_ __TA_GUARDED(domain_->token()) =
      zx::duration(0);
  uint32_t notifications_per_ring_ __TA_GUARDED(domain_->token()) = 0;
  zx::time target_notification_time_ __TA_GUARDED(domain_->token()) =
      zx::time(0);

  bool using_alt_notifications_ __TA_GUARDED(domain_->token());
  zx::duration alt_notification_period_ __TA_GUARDED(domain_->token()) =
      zx::duration(0);
  uint32_t alt_notifications_per_ring_ __TA_GUARDED(domain_->token()) = 0;
  zx::time target_alt_notification_time_ __TA_GUARDED(domain_->token()) =
      zx::time(0);

  uint32_t bytes_per_sec_ __TA_GUARDED(domain_->token()) = 0;
  uint32_t frame_rate_ __TA_GUARDED(domain_->token()) = 0;
  audio_sample_format_t sample_format_ __TA_GUARDED(domain_->token()) = 0;
  uint32_t num_channels_ __TA_GUARDED(domain_->token()) = 0;

  VirtualAudioDeviceImpl* parent_ __TA_GUARDED(domain_->token());

  fbl::Mutex wakeup_queue_lock_ __TA_ACQUIRED_AFTER(domain_->token());

  // TODO(mpuryear): Refactor to a single queue of lambdas to dedupe this code.
  fbl::RefPtr<dispatcher::WakeupEvent> plug_change_wakeup_;
  std::deque<PlugType> plug_queue_ __TA_GUARDED(wakeup_queue_lock_);

  fbl::RefPtr<dispatcher::WakeupEvent> gain_request_wakeup_;
  std::deque<fuchsia::virtualaudio::Device::GetGainCallback> gain_queue_
      __TA_GUARDED(wakeup_queue_lock_);

  fbl::RefPtr<dispatcher::WakeupEvent> format_request_wakeup_;
  std::deque<fuchsia::virtualaudio::Device::GetFormatCallback> format_queue_
      __TA_GUARDED(wakeup_queue_lock_);

  fbl::RefPtr<dispatcher::WakeupEvent> buffer_request_wakeup_;
  std::deque<fuchsia::virtualaudio::Device::GetBufferCallback> buffer_queue_
      __TA_GUARDED(wakeup_queue_lock_);

  fbl::RefPtr<dispatcher::WakeupEvent> position_request_wakeup_;
  std::deque<fuchsia::virtualaudio::Device::GetPositionCallback> position_queue_
      __TA_GUARDED(wakeup_queue_lock_);

  fbl::RefPtr<dispatcher::WakeupEvent> set_notifications_wakeup_;
  std::deque<uint32_t> notifs_queue_ __TA_GUARDED(wakeup_queue_lock_);
};

}  // namespace virtual_audio

#endif  // GARNET_DRIVERS_AUDIO_VIRTUAL_AUDIO_VIRTUAL_AUDIO_STREAM_H_
