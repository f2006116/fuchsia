// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_ESCHER_COMMON_DEMO_HARNESS_LINUX_H_
#define SRC_UI_EXAMPLES_ESCHER_COMMON_DEMO_HARNESS_LINUX_H_

#include "src/ui/examples/escher/common/demo_harness.h"

class DemoHarnessLinux : public DemoHarness {
 public:
  DemoHarnessLinux(WindowParams window_params);

  void Run(Demo* demo) override;

 private:
  // |DemoHarness|
  // Called by Init().
  void InitWindowSystem() override;
  vk::SurfaceKHR CreateWindowAndSurface(
      const WindowParams& window_params) override;

  // |DemoHarness|
  // Called by Init() via CreateInstance().
  void AppendPlatformSpecificInstanceExtensionNames(
      InstanceParams* params) override;
  void AppendPlatformSpecificDeviceExtensionNames(
      std::set<std::string>* names) override;

  // Called by Shutdown().
  void ShutdownWindowSystem() override;
};

#endif  // SRC_UI_EXAMPLES_ESCHER_COMMON_DEMO_HARNESS_LINUX_H_
