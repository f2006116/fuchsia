// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <ddk/debug.h>
#include <hwreg/bitfields.h>

namespace pci {
namespace config {

// Fields correspond to the name in the PCI Local Bus spec section 6.2.
struct Command {
    uint16_t value;
    // 15-11 are reserved preserve.
    DEF_SUBBIT(value, 10, interrupt_disable);
    DEF_SUBBIT(value, 9, fast_back_to_back_enable);
    DEF_SUBBIT(value, 8, serr_enable);
    // 7 is reserved preserve.
    DEF_SUBBIT(value, 6, parity_error_response);
    DEF_SUBBIT(value, 5, vga_palette_snoop);
    DEF_SUBBIT(value, 4, memory_write_and_invalidate_enable);
    DEF_SUBBIT(value, 3, special_cycles);
    DEF_SUBBIT(value, 2, bus_master);
    DEF_SUBBIT(value, 1, memory_space);
    DEF_SUBBIT(value, 0, io_space);
};

} // namespace config
} // namespace pci

// Switch for easy enabling of output during unit tests where we
// won't have driver log values set.
#if 1
#define pci_tracef(...) zxlogf(TRACE, "pci: " __VA_ARGS__)
#define pci_errorf(...) zxlogf(ERROR, "pci: " __VA_ARGS__)
#define pci_infof(...) zxlogf(INFO, "pci: " __VA_ARGS__)
#else
#define pci_tracef(...) printf("pci: " __VA_ARGS__)
#define pci_errorf(...) printf("pci: " __VA_ARGS__)
#define pci_infof(...) printf("pci: " __VA_ARGS__)
#endif

/*
 * PCI access return codes
 */
#define _PCI_SUCCESSFUL 0x00
#define _PCI_FUNC_NOT_SUPPORTED 0x81
#define _PCI_BAD_VENDOR_ID 0x83
#define _PCI_DEVICE_NOT_FOUND 0x86
#define _PCI_BAD_REGISTER_NUMBER 0x87
#define _PCI_SET_FAILED 0x88
#define _PCI_BUFFER_TOO_SMALL 0x89

#define PCI_CONFIG_HDR_SIZE (64u)
#define PCI_BASE_CONFIG_SIZE (256u)
#define PCI_EXT_CONFIG_SIZE (4096u)
/*
 * PCI configuration space offsets
 */
#define PCI_CONFIG_VENDOR_ID 0x00
#define PCI_CONFIG_DEVICE_ID 0x02
#define PCI_CONFIG_COMMAND 0x04
#define PCI_CONFIG_STATUS 0x06
#define PCI_CONFIG_REVISION_ID 0x08
#define PCI_CONFIG_CLASS_CODE 0x09
#define PCI_CONFIG_CLASS_CODE_INTR 0x09
#define PCI_CONFIG_CLASS_CODE_SUB 0x0a
#define PCI_CONFIG_CLASS_CODE_BASE 0x0b
#define PCI_CONFIG_CACHE_LINE_SIZE 0x0c
#define PCI_CONFIG_LATENCY_TIMER 0x0d
#define PCI_CONFIG_HEADER_TYPE 0x0e
#define PCI_CONFIG_BIST 0x0f
#define PCI_CONFIG_BASE_ADDRESSES 0x10
#define PCI_CONFIG_CARDBUS_CIS_PTR 0x28
#define PCI_CONFIG_SUBSYS_VENDOR_ID 0x2c
#define PCI_CONFIG_SUBSYS_ID 0x2e
#define PCI_CONFIG_EXP_ROM_ADDRESS 0x30
#define PCI_CONFIG_CAPABILITIES 0x34
#define PCI_CONFIG_INTERRUPT_LINE 0x3c
#define PCI_CONFIG_INTERRUPT_PIN 0x3d
#define PCI_CONFIG_MIN_GRANT 0x3e
#define PCI_CONFIG_MAX_LATENCY 0x3f

/*
 * PCI header type register bits
 */
#define PCI_HEADER_TYPE_MASK 0x7f
#define PCI_HEADER_TYPE_MULTI_FN 0x80

/*
 * PCI header types
 */
#define PCI_HEADER_TYPE_STANDARD 0x00
#define PCI_HEADER_TYPE_BRIDGE 0x01
#define PCI_HEADER_TYPE_CARD_BUS 0x02

/*
 * PCI command register bits
 */
#define PCI_COMMAND_IO_EN 0x0001
#define PCI_COMMAND_MEM_EN 0x0002
#define PCI_COMMAND_BUS_MASTER_EN 0x0004
#define PCI_COMMAND_SPECIAL_EN 0x0008
#define PCI_COMMAND_MEM_WR_INV_EN 0x0010
#define PCI_COMMAND_PAL_SNOOP_EN 0x0020
#define PCI_COMMAND_PERR_RESP_EN 0x0040
#define PCI_COMMAND_AD_STEP_EN 0x0080
#define PCI_COMMAND_SERR_EN 0x0100
#define PCI_COMMAND_FAST_B2B_EN 0x0200

/**
 * The maximum possible number of standard capabilities for a PCI
 * device/function is 48.  This comes from the facts that...
 *
 * ++ There are 256 bytes in the standard configuration space.
 * ++ The first 64 bytes are used by the standard configuration header, leaving
 *    192 bytes for capabilities.
 * ++ Even though the capability header is only 2 bytes long, it must be aligned
 *    on a 4 byte boundary.  The means that one can pack (at most) 192 / 4 == 48
 *    properly aligned standard PCI capabilities.
 *
 * Similar logic may be applied to extended capabilities which must also be 4
 * byte aligned, but exist in the region after the standard configuration block.
 */
#define PCI_CAPABILITY_ALIGNMENT (4u)

#define PCI_MAX_CAPABILITIES ((PCI_BASE_CONFIG_SIZE - PCI_STANDARD_CONFIG_HDR_SIZE) / PCI_CAPABILITY_ALIGNMENT)
#define PCI_CAP_PTR_NULL (0u)
#define PCI_CAP_PTR_MIN_VALID (PCI_STANDARD_CONFIG_HDR_SIZE)
#define PCI_CAP_PTR_MAX_VALID (PCI_BASE_CONFIG_SIZE - PCI_CAPABILITY_ALIGNMENT)
#define PCI_CAP_PTR_ALIGNMENT (2u)

#define PCIE_EXT_CAP_PTR_NULL (0u)
#define PCIE_EXT_CAP_PTR_MIN_VALID (PCI_BASE_CONFIG_SIZE)
#define PCIE_EXT_CAP_PTR_MAX_VALID (PCIE_EXTENDED_CONFIG_SIZE - PCI_CAPABILITY_ALIGNMENT)
#define PCIE_EXT_CAP_PTR_ALIGNMENT (4u)
#define PCIE_MAX_EXT_CAPABILITIES ((PCIE_EXTENDED_CONFIG_SIZE - PCI_BASE_CONFIG_SIZE) / PCI_CAPABILITY_ALIGNMENT)

/*
 * PCI BAR register masks and constants
 */
#define PCI_BAR_IO_TYPE_MASK (0x00000001)
#define PCI_BAR_IO_TYPE_MMIO (0x00000000)
#define PCI_BAR_IO_TYPE_PIO (0x00000001)

#define PCI_BAR_MMIO_TYPE_MASK (0x00000006)
#define PCI_BAR_MMIO_TYPE_32BIT (0x00000000)
#define PCI_BAR_MMIO_TYPE_64BIT (0x00000004)

#define PCI_BAR_MMIO_PREFETCH_MASK (0x00000008)
#define PCI_BAR_MMIO_ADDR_MASK (0xFFFFFFF0)
#define PCI_BAR_PIO_ADDR_MASK (0xFFFFFFFC)

/*
 * Extra bits used in the CFG command and status registers defined by PCIe.  See
 * the PCIe Base Specification, sections 7.5.1.1 and 7.5.1.2
 */
#define PCIE_CFG_COMMAND_INT_DISABLE ((uint16_t)(1 << 10))
#define PCIE_CFG_STATUS_INT_STS ((uint16_t)(1 << 3))

#ifdef __x86_64__
constexpr bool PCI_HAS_IO_ADDR_SPACE = true;
constexpr uint64_t PCI_PIO_ADDR_SPACE_MASK = 0xFFFF;
constexpr uint64_t PCI_PIO_ADDR_SPACE_SIZE = 0x10000;
#else  // non-x86
constexpr bool PCI_HAS_IO_ADDR_SPACE = false;
constexpr uint64_t PCI_PIO_ADDR_SPACE_MASK = 0xFFFFFFFF;
constexpr uint64_t PCI_PIO_ADDR_SPACE_SIZE = 0x100000000;
#endif // #if defined(__x86_64__)
