// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/thread_controller_test.h"

#include "src/developer/debug/zxdb/client/process_impl.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/system.h"
#include "src/developer/debug/zxdb/client/target_impl.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/symbols/mock_module_symbols.h"

namespace zxdb {

// static
const uint64_t ThreadControllerTest::kUnsymbolizedModuleAddress = 0x4000000;

// static
const uint64_t ThreadControllerTest::kSymbolizedModuleAddress = 0x5000000;

ThreadControllerTest::ThreadControllerTest() = default;
ThreadControllerTest::~ThreadControllerTest() = default;

void ThreadControllerTest::SetUp() {
  RemoteAPITest::SetUp();
  process_ = InjectProcess(0x1234);
  thread_ = InjectThread(process_->GetKoid(), 0x7890);

  // Inject a mock module symbols.
  std::string build_id("abcd");  // Identifies the module below.
  auto module_symbols = std::make_unique<MockModuleSymbols>("file.so");
  module_symbols_ = module_symbols.get();  // Save pointer for tests.
  symbol_module_ref_ = session().system().GetSymbols()->InjectModuleForTesting(
      build_id, std::move(module_symbols));

  // Make the process load the mocked module symbols and the other one with no
  // symbols.
  std::vector<debug_ipc::Module> modules;
  debug_ipc::Module unsym_load;
  unsym_load.name = "nosym";
  unsym_load.base = kUnsymbolizedModuleAddress;
  unsym_load.build_id = "zzyzx";
  modules.push_back(unsym_load);

  debug_ipc::Module sym_load;
  sym_load.name = "sym";
  sym_load.base = kSymbolizedModuleAddress;
  sym_load.build_id = build_id;
  modules.push_back(sym_load);

  TargetImpl* target = session().system_impl().GetTargetImpls()[0];
  target->process()->OnModules(modules, std::vector<uint64_t>());
}

std::unique_ptr<RemoteAPI> ThreadControllerTest::GetRemoteAPIImpl() {
  auto remote_api = std::make_unique<MockRemoteAPI>();
  mock_remote_api_ = remote_api.get();
  return remote_api;
}

}  // namespace zxdb
