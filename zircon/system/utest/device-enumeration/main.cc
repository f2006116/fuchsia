// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <fbl/algorithm.h>
#include <fbl/unique_fd.h>
#include <fuchsia/sysinfo/c/fidl.h>
#include <lib/devmgr-integration-test/fixture.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <unittest/unittest.h>
#include <zircon/status.h>

using devmgr_integration_test::RecursiveWaitForFile;

namespace {

enum class Board {
    kQemu,
    kVim2,
    kAstro,
    kCleo,
    kSherlock,
    kMt8167sRef,
    kMsm8x53Som,
    kAs370,
    kVisalia,
    kUnknown,
};

Board GetBoardType() {
    constexpr char kSysInfoPath[] = "/dev/misc/sysinfo";
    fbl::unique_fd sysinfo(open(kSysInfoPath, O_RDWR));
    if (!sysinfo) {
        return Board::kUnknown;
    }
    zx::channel channel;
    if (fdio_get_service_handle(sysinfo.release(), channel.reset_and_get_address()) != ZX_OK) {
        return Board::kUnknown;
    }

    char board_name[fuchsia_sysinfo_SYSINFO_BOARD_NAME_LEN + 1];
    zx_status_t status;
    size_t actual_size;
    zx_status_t fidl_status = fuchsia_sysinfo_DeviceGetBoardName(
        channel.get(), &status, board_name, sizeof(board_name), &actual_size);
    if (fidl_status != ZX_OK || status != ZX_OK) {
        return Board::kUnknown;
    }
    board_name[actual_size] = '\0';

    printf("Found board %s\n", board_name);

    if (!strcmp(board_name, "qemu")) {
        return Board::kQemu;
    } else if (!strcmp(board_name, "vim2")) {
        return Board::kVim2;
    } else if (!strcmp(board_name, "astro")) {
        return Board::kAstro;
    } else if (!strcmp(board_name, "cleo")) {
        return Board::kCleo;
    } else if (!strcmp(board_name, "sherlock")) {
        return Board::kSherlock;
    } else if (!strcmp(board_name, "mt8167s_ref")) {
        return Board::kMt8167sRef;
    } else if (!strcmp(board_name, "msm8x53-som")) {
        return Board::kMsm8x53Som;
    } else if (!strcmp(board_name, "as370")) {
        return Board::kAs370;
    } else if (!strcmp(board_name, "visalia")) {
        return Board::kVisalia;
    }

    return Board::kUnknown;
}

bool TestRunner(const char** device_paths, size_t paths_num) {
    BEGIN_HELPER;

    fbl::unique_fd devfs_root(open("/dev", O_RDWR));
    ASSERT_TRUE(devfs_root);

    fbl::unique_fd fd;
    for (size_t i = 0; i < paths_num; ++i) {
        printf("Checking %s\n", device_paths[i]);
        EXPECT_EQ(RecursiveWaitForFile(devfs_root, device_paths[i], &fd),
                  ZX_OK, device_paths[i]);
    }
    END_HELPER;
}

bool qemu_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/qemu-bus",
        "sys/platform/00:00:6/rtc",
        "sys/pci/00:00.0",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool vim2_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/vim",
        "sys/platform/00:00:1b/sysmem",
        "sys/platform/05:02:1/aml-gxl-gpio",
#if USE_GPIO_TEST
        "sys/platform/00:00:8/gpio-test",
#endif
        "sys/platform/05:00:2/aml-i2c",
        "sys/platform/05:02:4/clocks",
        "sys/platform/05:00:10/aml-canvas",
        // TODO(ZX-4069): Test SDMMC binding.
        // "sys/platform/05:00:3/aml-uart/serial/bt-transport-uart/bcm-hci",
        // "sys/platform/05:00:6/aml-sd-emmc/sdio",
        "sys/platform/05:00:2/aml-i2c/i2c/i2c-1-81/rtc",
        "sys/platform/05:00:2/aml-i2c/i2c/i2c-0-70/led2472g",
        "sys/platform/00:00:2/xhci/usb-bus",
        "sys/platform/05:02:17/aml-gpu",
        "dwmac/eth_phy/phy_null_device",
        "dwmac/Designware MAC/ethernet",
        "display/vim2-display/display-controller",
        "vim-thermal/vim-thermal",
        "wifi",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool astro_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/aml-bus",
        "sys/platform/05:03:1/aml-axg-gpio",
        "sys/platform/00:00:13/hid-buttons/hid-device-000",
        "sys/platform/05:00:2/aml-i2c",
        "sys/platform/05:03:17/aml-gpu",
        "sys/platform/05:00:18/aml-usb-phy-v2",

        // XHCI driver will not be loaded if we are in USB peripheral mode.
        // "xhci/xhci/usb-bus",

        // TODO(ZX-4087): Astro can have one of two possible touch screens
        // so we can't just test that one of them is bound. That is why the
        // following test is disabled.
        // "sys/platform/03:03:5/gt92xx HidDevice/hid-device-000",
        "sys/platform/05:00:2/aml-i2c/i2c/i2c-2-44/ti-lp8556",
        "display/astro-display/display-controller",
        "sys/platform/05:00:10/aml-canvas",
        "sys/platform/00:00:e/optee-tz",
        "sys/platform/05:03:e/amlogic_video",
        "sys/platform/00:00:f/fallback-rtc",
        "sys/platform/05:00:f/aml-raw_nand/nand/tpl/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/fts/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/factory/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/zircon-b/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/zircon-a/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/zircon-r/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/fvm/ftl/block/fvm/blobfs-p-1/block",
        "sys/platform/05:00:f/aml-raw_nand/nand/fvm/ftl/block/fvm/minfs-p-2/block/zxcrypt/unsealed/block",
        "sys/platform/05:00:f/aml-raw_nand/nand/sys-config/skip-block",
        "sys/platform/05:00:f/aml-raw_nand/nand/migration/skip-block",
        "sys/platform/05:00:7/aml-sd-emmc/sdmmc",
        "wifi",
        "tcs3400-light/tcs-3400/hid-device-000",
        "sys/platform/05:03:11/clocks",
        "sys/platform/05:03:a/thermal",
        "AstroAudio/astro-audio-out",
        "sys/platform/05:03:13/astro-audio-in",
        //"sys/platform/05:05:3/aml-uart/serial/bt-transport-uart/bcm-hci",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool cleo_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/mt8167s_ref",
        "sys/platform/0d:00:1/mt8167-gpio",
        "sys/platform/0d:00:6/mt8167-i2c",
        "sys/platform/0d:00:8/mtk-clk",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-000/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-001/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-002/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-003/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-004/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-005/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-006/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-007/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-008/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-009/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-010/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-011/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-012/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-013/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-014/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-015/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-016/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-017/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-018/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-019/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-020/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-021/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-022/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-023/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-024/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-025/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-026/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-027/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-028/block",
        "sys/platform/0d:00:4/mtk-sdmmc",
        "sys/platform/00:00:13/hid-buttons/hid-device-000",
        "sys/platform/0d:00:14/mt-usb/usb-peripheral/function-000/cdc-eth-function/ethernet",
        "touch/focaltouch HidDevice/hid-device-000",
        "sys/platform/0d:00:9/mtk-thermal",
        "sys/platform/0d:00:6/mt8167-i2c/i2c/i2c-0-83/ltr-578als/hid-device-000",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool sherlock_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/sherlock",
        "sys/platform/05:04:1/aml-axg-gpio",
        "sys/platform/05:00:14/clocks",
        "sys/platform/05:00:2/aml-i2c",
        "sys/platform/05:00:10/aml-canvas",
        "sys/platform/05:03:a/thermal",
        "sys/platform/00:00:1e/dw-dsi",
        "display/astro-display/display-controller",
        "sys/platform/05:00:18/aml-usb-phy-v2",

        // XHCI driver will not be loaded if we are in USB peripheral mode.
        // "xhci/xhci/usb-bus",

        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-000/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-002/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-003/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-004/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-005/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-006/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-007/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-008/block/fvm/blobfs-p-1/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-008/block/fvm/minfs-p-2/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-009/block",
        "sys/platform/05:00:8/aml-sd-emmc/sdmmc/sdmmc-mmc/block/part-010/block",
        "sys/platform/05:00:6/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-1",
        "sys/platform/05:00:6/aml-sd-emmc/sdmmc/sdmmc-sdio/sdmmc-sdio-2",
        "wifi",
        "sys/platform/05:04:15/aml-mipi",
        "sys/platform/12:02:2/gdc",
        "imx227-sensor",
        "isp",
        "sys/platform/05:04:e/amlogic_video",
        "sys/platform/05:04:17/aml-gpu",
        "sys/platform/05:04:16/sherlock-audio-in",
        "SherlockAudio",
        "ft5726-touch",
        "sys/platform/00:00:e/optee-tz",
        "sys/platform/00:00:f/fallback-rtc",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool mt8167s_ref_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/mt8167s_ref",
        "sys/platform/0d:00:1/mt8167-gpio",
        "sys/platform/0d:00:6/mt8167-i2c",
        "sys/platform/0d:00:8/mtk-clk",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-000/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-001/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-002/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-003/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-004/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-005/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-006/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-007/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-008/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-009/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-010/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-011/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-012/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-013/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-014/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-015/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-016/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-017/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-018/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-019/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-020/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-021/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-022/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-023/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-024/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-025/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-026/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-027/block",
        "sys/platform/0d:00:2/mtk-sdmmc/sdmmc/sdmmc-mmc/block/part-028/block",
        "sys/platform/0d:00:4/mtk-sdmmc",
        "sys/platform/00:00:13/hid-buttons/hid-device-000",
        "sys/platform/0d:00:14/mt-usb/usb-peripheral/function-000/cdc-eth-function/ethernet",
        "sys/platform/0d:00:9/mtk-thermal",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool msm8x53_som_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/msm8x53",
        "sys/platform/13:01:1",
        "sys/platform/13:00:3/msm8x53-sdhci",
        "sys/platform/13:00:2/qcom-pil",
        "sys/platform/13:01:4/msm-clk",
        "sys/platform/13:01:5/msm8x53-power"};

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool as370_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/as370",
        "sys/platform/14:01:1",
        "sys/platform/14:01:1/as370-gpio",
        "sys/platform/00:00:9",
        "sys/platform/00:00:9/dw-i2c",
        "sys/platform/14:01:2/as370-usb-phy",
        "dwc2-usb",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

bool visalia_enumeration_test() {
    BEGIN_TEST;
    static const char* kDevicePaths[] = {
        "sys/platform/as370",
        "sys/platform/14:01:1",
        "sys/platform/14:01:1/as370-gpio",
        "sys/platform/00:00:9",
        "sys/platform/00:00:9/dw-i2c",
        "sys/platform/14:01:2/as370-usb-phy",
        "dwc2-usb",
    };

    ASSERT_TRUE(TestRunner(kDevicePaths, fbl::count_of(kDevicePaths)));

    END_TEST;
}

#define MAKE_TEST_CASE(name)          \
    BEGIN_TEST_CASE(name)             \
    RUN_TEST(name##_enumeration_test) \
    END_TEST_CASE(name)               \
    test_case_element* test_case_##name = TEST_CASE_ELEMENT(name)

MAKE_TEST_CASE(qemu);
MAKE_TEST_CASE(vim2);
MAKE_TEST_CASE(astro);
MAKE_TEST_CASE(cleo);
MAKE_TEST_CASE(sherlock);
MAKE_TEST_CASE(mt8167s_ref);
MAKE_TEST_CASE(msm8x53_som);
MAKE_TEST_CASE(as370);
MAKE_TEST_CASE(visalia);

#undef MAKE_TEST_CASE

} // namespace

int main(int argc, char** argv) {
    switch (GetBoardType()) {
    case Board::kQemu:
        return unittest_run_one_test(test_case_qemu, TEST_ALL) ? 0 : -1;
    case Board::kVim2:
        return unittest_run_one_test(test_case_vim2, TEST_ALL) ? 0 : -1;
    case Board::kAstro:
        return unittest_run_one_test(test_case_astro, TEST_ALL) ? 0 : -1;
    case Board::kCleo:
        return unittest_run_one_test(test_case_cleo, TEST_ALL) ? 0 : -1;
    case Board::kSherlock:
        return unittest_run_one_test(test_case_sherlock, TEST_ALL) ? 0 : -1;
    case Board::kMt8167sRef:
        return unittest_run_one_test(test_case_mt8167s_ref, TEST_ALL) ? 0 : -1;
    case Board::kMsm8x53Som:
        return unittest_run_one_test(test_case_msm8x53_som, TEST_ALL) ? 0 : -1;
    case Board::kAs370:
        return unittest_run_one_test(test_case_as370, TEST_ALL) ? 0 : -1;
    case Board::kVisalia:
        return unittest_run_one_test(test_case_visalia, TEST_ALL) ? 0 : -1;
    case Board::kUnknown:
        return 0;
    }
}
