// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "host.h"

#include "fidl/host_server.h"
#include "gatt_host.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/device_wrapper.h"
#include "src/connectivity/bluetooth/core/bt-host/hci/transport.h"

using namespace bt;

namespace bthost {

Host::Host(const bt_hci_protocol_t& hci_proto) : hci_proto_(hci_proto) {}

Host::~Host() {}

bool Host::Initialize(InitCallback callback) {
  auto dev = std::make_unique<hci::DdkDeviceWrapper>(hci_proto_);
  auto hci = hci::Transport::Create(std::move(dev));
  if (!hci)
    return false;

  bt_log(TRACE, "bt-host", "initializing HCI");
  if (!hci->Initialize()) {
    bt_log(ERROR, "bt-host", "failed to initialize HCI transport");
    return false;
  }

  data_domain_ = data::Domain::Create(hci, "bt-host (data)");
  if (!data_domain_)
    return false;

  gatt_host_ = GattHost::Create("bt-host (gatt)");
  if (!gatt_host_)
    return false;

  gap_ =
      std::make_unique<gap::Adapter>(hci, data_domain_, gatt_host_->profile());
  if (!gap_)
    return false;

  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(data_domain_);

  // Called when the GATT profile is ready. After the GATT profile is ready,
  // we initialize the generic access service as it depends on the GATT
  // profile to be ready.
  auto gatt_init_callback = [gap = gap_->AsWeakPtr(),
                             init_callback = callback.share()]() {
    bt_log(TRACE, "bt-host", "GATT init complete (success)");

    if (gap) {
      bt_log(TRACE, "bt-host", "Initializing Generic Access Service...");
      gap->InitializeService();
    }

    init_callback(true);
  };

  // Called when the GAP layer is ready. We initialize the GATT profile after
  // initial setup in GAP. The data domain will be initialized by GAP because it
  // both sets up the HCI ACL data channel that L2CAP relies on and registers
  // L2CAP services.
  auto gap_init_callback = [gatt_host = gatt_host_,
                            init_callback = std::move(callback),
                            gatt_init_callback = std::move(gatt_init_callback)](
                               bool success) mutable {
    bt_log(TRACE, "bt-host", "GAP init complete (%s)",
           (success ? "success" : "failure"));

    if (success) {
      bt_log(TRACE, "bt-host", "Initializing GATT...");
      gatt_host->Initialize(std::move(gatt_init_callback));
    } else {
      init_callback(false);
    }
  };

  bt_log(TRACE, "bt-host", "Initializing GAP...");
  return gap_->Initialize(std::move(gap_init_callback), [] {
    bt_log(TRACE, "bt-host", "bt-host: HCI transport has closed");
  });
}

void Host::ShutDown() {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  bt_log(TRACE, "bt-host", "shutting down");

  if (!gap_) {
    bt_log(TRACE, "bt-host", "already shut down");
    return;
  }

  // Closes all FIDL channels owned by |host_server_|.
  host_server_ = nullptr;

  // This shuts down the GATT profile and all of its clients.
  gatt_host_->ShutDown();
  gap_->ShutDown();
  data_domain_->ShutDown();

  // Make sure that |gap_| gets shut down and destroyed on its creation thread
  // as it is not thread-safe.
  gap_ = nullptr;
  data_domain_ = nullptr;
  gatt_host_ = nullptr;
}

void Host::BindHostInterface(zx::channel channel) {
  ZX_DEBUG_ASSERT(thread_checker_.IsCreationThreadCurrent());
  if (host_server_) {
    bt_log(WARN, "bt-host", "Host interface channel already open!");
    return;
  }

  ZX_DEBUG_ASSERT(gap_);
  ZX_DEBUG_ASSERT(gatt_host_);

  host_server_ = std::make_unique<HostServer>(std::move(channel),
                                              gap_->AsWeakPtr(), gatt_host_);
  host_server_->set_error_handler([this](zx_status_t status) {
    ZX_DEBUG_ASSERT(host_server_);
    bt_log(TRACE, "bt-host", "Host interface disconnected");
    host_server_ = nullptr;
  });
}

}  // namespace bthost
