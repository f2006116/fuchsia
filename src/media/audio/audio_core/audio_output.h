// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_

#include <deque>
#include <memory>
#include <set>
#include <thread>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/fwd_decls.h"

namespace media::audio {

class DriverRingBuffer;

class AudioOutput : public AudioDevice {
 public:
  // Minimum clock lead time (in nanoseconds) for this output
  int64_t min_clock_lead_time_nsec() const { return min_clock_lead_time_nsec_; }

 protected:
  explicit AudioOutput(AudioDeviceManager* manager);

  int64_t min_clock_lead_time_nsec_ = 0;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_AUDIO_OUTPUT_H_