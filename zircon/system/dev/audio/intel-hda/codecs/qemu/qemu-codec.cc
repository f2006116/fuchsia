// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/driver.h>
#include <ddk/platform-defs.h>
#include <fbl/auto_lock.h>

#include <dispatcher-pool/dispatcher-thread-pool.h>

#include "debug-logging.h"
#include "qemu-codec.h"
#include "qemu-stream.h"

namespace audio {
namespace intel_hda {
namespace codecs {

class QemuInputStream : public QemuStream  {
public:
    static constexpr uint32_t STREAM_ID = 2;
    static constexpr uint16_t CONVERTER_NID = 4;
    QemuInputStream() : QemuStream(STREAM_ID, true, CONVERTER_NID) { }
};

class QemuOutputStream : public QemuStream  {
public:
    static constexpr uint32_t STREAM_ID = 1;
    static constexpr uint16_t CONVERTER_NID = 2;
    QemuOutputStream() : QemuStream(STREAM_ID, false, CONVERTER_NID) { }
};

void QemuCodec::PrintDebugPrefix() const {
    printf("QEMUCodec : ");
}

zx_status_t QemuCodec::Create(void* ctx, zx_device_t* parent) {
    fbl::RefPtr<QemuCodec> codec = fbl::AdoptRef(new QemuCodec);
    ZX_DEBUG_ASSERT(codec != nullptr);
    return codec->Init(parent);
}

zx_status_t QemuCodec::Init(zx_device_t* codec_dev) {
    zx_status_t res = Bind(codec_dev, "qemu-codec");
    if (res != ZX_OK)
        return res;

    res = Start();
    if (res != ZX_OK) {
        Shutdown();
        return res;
    }

    return ZX_OK;
}

zx_status_t QemuCodec::Start() {
    zx_status_t res;

    auto output = fbl::AdoptRef<QemuStream>(new QemuOutputStream());
    res = ActivateStream(output);
    if (res != ZX_OK) {
        LOG("Failed to activate output stream (res %d)!", res);
        return res;
    }

    auto input = fbl::AdoptRef<QemuStream>(new QemuInputStream());
    res = ActivateStream(input);
    if (res != ZX_OK) {
        LOG("Failed to activate input stream (res %d)!", res);
        return res;
    }

    return ZX_OK;
}

static constexpr zx_driver_ops_t driver_ops = []() {
    zx_driver_ops_t ops = {};
    ops.version = DRIVER_OPS_VERSION;
    ops.bind = QemuCodec::Create;
    return ops;
}();

}  // namespace codecs
}  // namespace audio
}  // namespace intel_hda

// clang-format off
ZIRCON_DRIVER_BEGIN(qemu_ihda_codec, audio::intel_hda::codecs::driver_ops, "zircon", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_IHDA_CODEC),
    BI_ABORT_IF(NE, BIND_IHDA_CODEC_VID, 0x1af4),
    BI_MATCH_IF(EQ, BIND_IHDA_CODEC_DID, 0x0022),
ZIRCON_DRIVER_END(qemu_ihda_codec)
// clang-format on
