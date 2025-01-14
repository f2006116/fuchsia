// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <usb/usb.h>
#include <memory>

namespace usb {

zx_status_t InterfaceList::Create(const ddk::UsbProtocolClient& client, bool skip_alt,
                                  std::optional<InterfaceList>* out) {
    usb_protocol_t proto;
    client.GetProto(&proto);

    usb_desc_iter_t iter;
    auto status = usb_desc_iter_init(&proto, &iter);
    if (status != ZX_OK) {
        return status;
    }

    out->emplace(iter, skip_alt);
    return ZX_OK;
}

InterfaceList::iterator InterfaceList::begin() const {
    if (!iter_.desc) {
        return end();
    }
    usb_desc_iter_t iter = iter_;
    const usb_interface_descriptor_t* intf = usb_desc_iter_next_interface(&iter, skip_alt_);
    return iterator(iter, skip_alt_, intf);
}

InterfaceList::const_iterator InterfaceList::cbegin() const {
    return static_cast<InterfaceList::const_iterator>(begin());
}

InterfaceList::iterator InterfaceList::end() const {
    usb_desc_iter_t init = {};
    return iterator(init, skip_alt_, nullptr);
}

InterfaceList::const_iterator InterfaceList::cend() const {
    return static_cast<InterfaceList::const_iterator>(end());
}

void Interface::Next(bool skip_alt) {
    descriptor_ = usb_desc_iter_next_interface(&iter_, skip_alt);
}

Interface::iterator Interface::begin() const {
    if (!iter_.desc) {
        return end();
    }
    usb_desc_iter_t iter = iter_;
    const usb_endpoint_descriptor_t* endpoint = usb_desc_iter_next_endpoint(&iter);
    return iterator(iter, endpoint);
}

Interface::const_iterator Interface::cbegin() const {
    return static_cast<Interface::const_iterator>(begin());
}

Interface::iterator Interface::end() const {
    usb_desc_iter_t init = {};
    return iterator(init, nullptr);
}

Interface::const_iterator Interface::cend() const {
    return static_cast<Interface::const_iterator>(begin());
}

} // namespace usb
