// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_GUEST_GUEST_CONFIG_H_
#define GARNET_BIN_GUEST_GUEST_CONFIG_H_

#include <functional>
#include <string>
#include <unordered_map>

#include <zircon/types.h>

class GuestConfig {
 public:
  GuestConfig();
  ~GuestConfig();

  const std::string& kernel_path() const { return kernel_path_; }
  const std::string& ramdisk_path() const { return ramdisk_path_; }
  const std::string& block_path() const { return block_path_; }
  const std::string& cmdline() const { return cmdline_; }
  zx_duration_t balloon_interval() const {
    return ZX_SEC(balloon_interval_seconds_);
  }
  uint32_t balloon_pages_threshold() const { return balloon_pages_threshold_; }
  bool balloon_demand_page() const { return balloon_demand_page_; }

 private:
  friend class GuestConfigParser;
  std::string kernel_path_;
  std::string ramdisk_path_;
  std::string block_path_;
  std::string cmdline_;
  uint32_t balloon_interval_seconds_ = 0;
  uint32_t balloon_pages_threshold_ = 0;
  bool balloon_demand_page_ = false;
};

class GuestConfigParser {
 public:
  using OptionHandler = std::function<zx_status_t(const std::string& name,
                                                  const std::string& value)>;
  GuestConfigParser(GuestConfig* config);
  ~GuestConfigParser();

  zx_status_t ParseArgcArgv(int argc, char** argv);
  zx_status_t ParseConfig(const std::string& data);

 private:
  GuestConfig* config_;

  using OptionMap = std::unordered_map<std::string, OptionHandler>;
  OptionMap options_;
};

#endif  // GARNET_BIN_GUEST_GUEST_CONFIG_H_
