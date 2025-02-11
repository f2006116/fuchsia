// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fs/pseudo-dir.h>
#include <fuchsia/device/manager/cpp/fidl.h>
#include <fuchsia/modular/testing/cpp/fidl.h>
#include <lib/fsl/vmo/strings.h>
#include <lib/modular_test_harness/cpp/fake_component.h>
#include <lib/modular_test_harness/cpp/test_harness_fixture.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/cpp/testing/test_with_environment.h>
#include <peridot/lib/util/pseudo_dir_server.h>

namespace {

class SessionmgrIntegrationTest : public modular::testing::TestHarnessFixture {
};

class MockAdmin : public fuchsia::device::manager::Administrator {
 public:
  bool suspend_called() { return suspend_called_; }

 private:
  void Suspend(uint32_t flags, SuspendCallback callback) override {
    ASSERT_FALSE(suspend_called_);
    suspend_called_ = true;
    ASSERT_EQ(fuchsia::device::manager::SUSPEND_FLAG_REBOOT, flags);
    callback(ZX_OK);
  }

  bool suspend_called_ = false;
};

class TestSessionShell : public modular::testing::FakeComponent {
 public:
  TestSessionShell(fit::function<void()> on_created,
                   fit::function<void()> on_destroyed)
      : on_created_(std::move(on_created)),
        on_destroyed_(std::move(on_destroyed)) {}

 protected:
  void OnCreate(fuchsia::sys::StartupInfo startup_info) override {
    on_created_();
  }

  void OnDestroy() override { on_destroyed_(); }

  fit::function<void()> on_created_;
  fit::function<void()> on_destroyed_;
};

TEST_F(SessionmgrIntegrationTest,
       RebootCalledIfSessionmgrCrashNumberReachesRetryLimit) {
  MockAdmin mock_admin;
  fidl::BindingSet<fuchsia::device::manager::Administrator> admin_bindings;

  TestSessionShell session_shell([] {}, [] {});
  modular::testing::TestHarnessBuilder builder;
  builder.InterceptSessionShell(session_shell.GetOnCreateHandler(),
                                {.url = modular::testing::GenerateFakeUrl()});
  builder.AddService(admin_bindings.GetHandler(&mock_admin));
  builder.BuildAndRun(test_harness());

  // kill session_shell
  for (int i = 0; i < 4; i++) {
    RunLoopUntil([&] { return session_shell.is_running(); });
    session_shell.Exit(0);
    RunLoopUntil([&] { return !session_shell.is_running(); });
  }
  // Validate suspend is invoked

  RunLoopUntil([&] { return mock_admin.suspend_called(); });
  EXPECT_TRUE(mock_admin.suspend_called());
}

}  // namespace
