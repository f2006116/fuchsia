// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/app_driver/cpp/agent_driver.h>
#include <lib/async-loop/cpp/loop.h>
#include <src/lib/fxl/logging.h>

#include "peridot/public/lib/integration_testing/cpp/reporting.h"
#include "peridot/public/lib/integration_testing/cpp/testing.h"
#include "peridot/tests/component_context/defs.h"

using ::modular::testing::Signal;
using ::modular::testing::TestPoint;

namespace {

// Cf. README.md for what this test does and how.
class TestApp {
 public:
  TestApp(modular::AgentHost* const agent_host) {
    modular::testing::Init(agent_host->component_context(), __FILE__);
  }

  // Called by AgentDriver.
  void Connect(
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> /*services*/) {
    Signal("two_agent_connected");
  }

  // Called by AgentDriver.
  void RunTask(fidl::StringPtr /*task_id*/,
               fit::function<void()> /*callback*/) {}

  TestPoint terminate_called_{"Terminate() called."};

  // Called by AgentDriver.
  void Terminate(fit::function<void()> done) {
    terminate_called_.Pass();
    modular::testing::Done(std::move(done));
  }

  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /*argc*/, const char** /*argv*/) {
  async::Loop loop(&kAsyncLoopConfigAttachToThread);
  auto context = sys::ComponentContext::Create();
  modular::AgentDriver<TestApp> driver(context.get(), [&loop] { loop.Quit(); });
  loop.Run();
  return 0;
}
