// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "apps/bluetooth/lib/hci/command_channel.h"
#include "apps/bluetooth/tools/lib/command_dispatcher.h"
#include "lib/ftl/macros.h"
#include "lib/ftl/memory/ref_ptr.h"
#include "lib/ftl/tasks/task_runner.h"

namespace hcitool {

class CommandData final {
 public:
  CommandData(bluetooth::hci::CommandChannel* cmd_channel, ftl::RefPtr<ftl::TaskRunner> task_runner)
      : cmd_channel_(cmd_channel), task_runner_(task_runner) {}

  bluetooth::hci::CommandChannel* cmd_channel() const { return cmd_channel_; }
  ftl::RefPtr<ftl::TaskRunner> task_runner() const { return task_runner_; }

 private:
  bluetooth::hci::CommandChannel* cmd_channel_;
  ftl::RefPtr<ftl::TaskRunner> task_runner_;
};

void RegisterCommands(const CommandData* data, bluetooth::tools::CommandDispatcher* dispatcher);

}  // namespace hcitool
