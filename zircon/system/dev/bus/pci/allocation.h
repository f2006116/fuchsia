// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include "allocation.h"
#include "ref_counted.h"
#include <ddktl/protocol/pciroot.h>
#include <fbl/macros.h>
#include <fbl/unique_ptr.h>
#include <lib/zx/resource.h>
#include <region-alloc/region-alloc.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

// PciAllocations and PciAllocators are concepts internal to UpstreamNodes which
// track address space allocations across roots and bridges. PciAllocator is an
// interface for roots and bridges to provide allocators to downstream bridges
// for their own allocations.
//
// === The Life of a PciAllocation ===
// Allocations at the top level of the bus driver are provided by a
// PciRootALlocator. This allocator serves requests from PCI Bridges & Devices
// that are just under the root complex and fulfills them by requesting space
// from the platform bus driver over the PciRoot protocol. When these bridges
// allocate their windows and bars from upstream they are requesting address
// space from the PciRootAllocator. The PciAllocations handed back to them
// contain a base/size pair, as well as a zx::resource corresponding to the
// given address space. A PciAllocation also has the ability to create a VMO
// constrained by the base / size it understands, which can be used for device bar
// allocations for drivers. If the requester of a PciAllocation is a Bridge
// fulfilling its bridge windows then the allocation is fed to the PciAllocators
// of that bridge. These allocators fulfill the same interface as
// PciRootAllocators, except they allow those bridges to provide for devices
// downstream of them.
//
// As a tree, the system looks like this:
//
//                               Root Protocol
//                                |         |
//                                v         v
//                           Bridge        Bridge
//                      (RootAllocator) (RootAllocator)
//                             |              |
//                             v              v
//                      RootAllocation  RootAllocation
//                            |               |
//                            v               v
//                          Bridge        Device (bar 4)
//                     (RegionAllocator)
//                      |          |
//                      v          v
//         RegionAllocation   RegionAllocation
//                 |                 |
//                 v                 v
//           Device (bar 2)     Device (bar 1)

namespace pci {

class PciAllocation {
public:
    // Delete Copy and Assignment ctors
    PciAllocation(const PciAllocation&) = delete;
    PciAllocation(PciAllocation&) = delete;
    PciAllocation& operator=(const PciAllocation&) = delete;
    PciAllocation& operator=(PciAllocation&&) = delete;

    virtual ~PciAllocation() = default;
    virtual zx_paddr_t base() const = 0;
    virtual size_t size() const = 0;
    // Create a VMO bounded by the base/size of this allocation using the
    // provided resource. This is used to provide VMOs for device BAR
    // allocations.
    zx_status_t CreateVmObject(std::unique_ptr<zx::vmo>* out_vmo) const;

protected:
    PciAllocation(zx::resource&& resource)
        : resource_(std::move(resource)) {}
    const zx::resource resource_;

private:
    // Allow PciRegionAllocator / Device to duplicate the resource for use further
    // down the bridge chain. The security implications of this are not a
    // concern because:
    // 1. The allocation object strictly bounds the VMO to the specified base & size
    // 2. The resource is already in the driver process's address space, so we're not
    //    leaking it anywhere out of band.
    // 3. Device needs to be able to pass a resource to DeviceProxy for setting
    //    IO permission bits.
    // This is only needed for PciRegionAllocators because PciRootAllocators do not
    // hold a backing PciAllocation object.
    friend class PciRegionAllocator;
    friend class Device;
    const zx::resource& resource() const { return resource_; }
};

class PciRootAllocation final : public PciAllocation {
public:
    PciRootAllocation(const ddk::PcirootProtocolClient client,
                      const pci_address_space_t type,
                      zx::resource&& resource,
                      zx_paddr_t base,
                      size_t size)
        : PciAllocation(std::move(resource)), pciroot_client_(client), type_(type), base_(base),
          size_(size) {}
    ~PciRootAllocation() final { pciroot_client_.FreeAddressSpace(base_, size_, type_); }

    zx_paddr_t base() const final { return base_; }
    size_t size() const final { return size_; }

private:
    const ddk::PcirootProtocolClient pciroot_client_;
    const pci_address_space_t type_;
    const zx_paddr_t base_;
    const size_t size_;
};

class PciRegionAllocation final : public PciAllocation {
public:
    PciRegionAllocation(zx::resource&& resource, RegionAllocator::Region::UPtr&& region)
        : PciAllocation(std::move(resource)), region_(std::move(region)) {}

    zx_paddr_t base() const final { return region_->base; }
    size_t size() const final { return region_->size; }

private:
    // The Region contains the base & size for the allocation through .base and .size
    const RegionAllocator::Region::UPtr region_;
};

// The base class for Root & Region allocators used by UpstreamNodes
class PciAllocator {
public:
    virtual ~PciAllocator() = default;
    // Delete Copy and Assignment ctors
    PciAllocator(const PciAllocator&) = delete;
    PciAllocator(PciAllocator&&) = delete;
    PciAllocator& operator=(const PciAllocator&) = delete;
    PciAllocator& operator=(PciAllocator&&) = delete;
    // Request a region of address space spanning from |base| to |base| + |size|.
    virtual zx_status_t GetRegion(zx_paddr_t base,
                                  size_t size,
                                  std::unique_ptr<PciAllocation>* out_alloc) = 0;
    // Request a region of address space of size |size| anywhere in the window.
    zx_status_t GetRegion(size_t size, std::unique_ptr<PciAllocation>* out_alloc) {
        return GetRegion(/* base */ 0, size, out_alloc);
    }
    virtual zx_status_t AddAddressSpace(std::unique_ptr<PciAllocation> alloc) = 0;

protected:
    PciAllocator() = default;
};

// PciRootAllocators are an implementation of PciAllocator designed
// to use the Pciroot protocol for allocation, fulfilling the requirements
// for a PciRoot to implement the UpstreamNode interface.
class PciRootAllocator : public PciAllocator {
public:
    PciRootAllocator(ddk::PcirootProtocolClient proto, pci_address_space_t type, bool low)
        : pciroot_(proto), type_(type), low_(low) {}
    zx_status_t GetRegion(zx_paddr_t base,
                          size_t size,
                          std::unique_ptr<PciAllocation>* alloc) final;
    zx_status_t AddAddressSpace(std::unique_ptr<PciAllocation> alloc);

private:
    // The bus driver outlives allocator objects.
    ddk::PcirootProtocolClient const pciroot_;
    const pci_address_space_t type_;
    // This denotes whether this allocator requests memory < 4GB. More detail
    // can be found in the explanation for mmio in root.h.
    const bool low_;
};

// PciRegionAllocators are a wrapper around RegionAllocators to allow Bridge
// objects to implement the UpstreamNode interface by using regions that they
// are provided by nodes further upstream. They hand out PciRegionAllocations
// which will release allocations back upstream if they go out of scope.
class PciRegionAllocator : public PciAllocator {
public:
    PciRegionAllocator() = default;
    zx_status_t GetRegion(zx_paddr_t base,
                          size_t size,
                          std::unique_ptr<PciAllocation>* alloc) final;
    zx_status_t AddAddressSpace(std::unique_ptr<PciAllocation> alloc) final;
    void SetRegionPool(RegionAllocator::RegionPool::RefPtr pool) { allocator_.SetRegionPool(pool); }

private:
    // This PciAllocation is the object handed to the bridge by the upstream node
    // and holds a reservation for that address space in the upstream bridge's window
    // for use downstream this bridge.
    std::unique_ptr<PciAllocation> backing_alloc_;
    RegionAllocator allocator_;
};

} // namespace pci
