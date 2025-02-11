// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/examples/ui/yuv_to_image_pipe/yuv_base_view.h"

#include <lib/images/cpp/images.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <src/lib/fxl/log_level.h>
#include <trace/event.h>

#include <iostream>

#include "src/ui/lib/yuv/yuv.h"

namespace yuv_to_image_pipe {

namespace {

constexpr uint32_t kShapeWidth = 640;
constexpr uint32_t kShapeHeight = 480;
constexpr float kDisplayHeight = 50;
constexpr float kInitialWindowXPos = 320;
constexpr float kInitialWindowYPos = 240;

}  // namespace

YuvBaseView::YuvBaseView(scenic::ViewContext context,
                         fuchsia::images::PixelFormat pixel_format)
    : BaseView(std::move(context), "YuvBaseView Example"),
      node_(session()),
      pixel_format_(pixel_format),
      stride_(static_cast<uint32_t>(
          kShapeWidth * images::StrideBytesPerWidthPixel(pixel_format_))) {
  FXL_VLOG(4) << "Creating View";

  // Create an ImagePipe and use it.
  uint32_t image_pipe_id = session()->AllocResourceId();
  session()->Enqueue(
      scenic::NewCreateImagePipeCmd(image_pipe_id, image_pipe_.NewRequest()));

  // Create a material that has our image pipe mapped onto it:
  scenic::Material material(session());
  material.SetTexture(image_pipe_id);
  session()->ReleaseResource(image_pipe_id);

  // Create a rectangle shape to display the YUV on.
  scenic::Rectangle shape(session(), kShapeWidth, kShapeHeight);

  node_.SetShape(shape);
  node_.SetMaterial(material);
  root_node().AddChild(node_);

  // Translation of 0, 0 is the middle of the screen
  node_.SetTranslation(kInitialWindowXPos, kInitialWindowYPos, -kDisplayHeight);
  InvalidateScene();
}

uint32_t YuvBaseView::AddImage() {
  ++next_image_id_;

  fuchsia::images::ImageInfo image_info{
      .width = kShapeWidth,
      .height = kShapeHeight,
      .stride = stride_,
      .pixel_format = pixel_format_,
  };
  uint64_t image_vmo_bytes = images::ImageSize(image_info);
  ::zx::vmo image_vmo;
  zx_status_t status = ::zx::vmo::create(image_vmo_bytes, 0, &image_vmo);
  if (status != ZX_OK) {
    FXL_LOG(FATAL) << "::zx::vmo::create() failed";
    FXL_NOTREACHED();
  }

  uint8_t* vmo_base;
  status = zx::vmar::root_self()->map(0, image_vmo, 0, image_vmo_bytes,
                                      ZX_VM_PERM_WRITE | ZX_VM_PERM_READ,
                                      reinterpret_cast<uintptr_t*>(&vmo_base));

  constexpr uint64_t kMemoryOffset = 0;
  image_pipe_->AddImage(next_image_id_, image_info, std::move(image_vmo),
                        kMemoryOffset, image_vmo_bytes,
                        fuchsia::images::MemoryType::HOST_MEMORY);
  image_vmos_[next_image_id_] = vmo_base;
  return next_image_id_;
}

void YuvBaseView::PaintImage(uint32_t image_id, uint8_t pixel_multiplier) {
  FXL_CHECK(image_vmos_.count(image_id));

  SetVmoPixels(image_vmos_[image_id], pixel_multiplier);
}

void YuvBaseView::PresentImage(uint32_t image_id) {
  FXL_CHECK(image_vmos_.count(image_id));
  TRACE_DURATION("gfx", "YuvBaseView::PresentImage");

  ::std::vector<::zx::event> acquire_fences;
  ::std::vector<::zx::event> release_fences;
  uint64_t now_ns = zx_clock_get_monotonic();
  TRACE_FLOW_BEGIN("gfx", "image_pipe_present_image", image_id);
  image_pipe_->PresentImage(
      image_id, now_ns, std::move(acquire_fences), std::move(release_fences),
      [](fuchsia::images::PresentationInfo presentation_info) {
        std::cout << "PresentImageCallback() called" << std::endl;
      });
}

void YuvBaseView::SetVmoPixels(uint8_t* vmo_base, uint8_t pixel_multiplier) {
  switch (pixel_format_) {
    case fuchsia::images::PixelFormat::BGRA_8:
      SetBgra8Pixels(vmo_base, pixel_multiplier);
      break;
    case fuchsia::images::PixelFormat::YUY2:
      SetYuy2Pixels(vmo_base, pixel_multiplier);
      break;
    case fuchsia::images::PixelFormat::NV12:
      SetNv12Pixels(vmo_base, pixel_multiplier);
      break;
    case fuchsia::images::PixelFormat::YV12:
      SetYv12Pixels(vmo_base, pixel_multiplier);
      break;
  }
}

void YuvBaseView::SetBgra8Pixels(uint8_t* vmo_base, uint8_t pixel_multiplier) {
  for (uint32_t y_iter = 0; y_iter < kShapeHeight; y_iter++) {
    double y = static_cast<double>(y_iter) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth; x_iter++) {
      double x = static_cast<double>(x_iter) / kShapeWidth;
      uint8_t y_value = GetYValue(x, y) * pixel_multiplier;
      uint8_t u_value = GetUValue(x, y) * pixel_multiplier;
      uint8_t v_value = GetVValue(x, y) * pixel_multiplier;
      yuv::YuvToBgra(y_value, u_value, v_value,
                     &vmo_base[y_iter * stride_ + x_iter * sizeof(uint32_t)]);
    }
  }
}

void YuvBaseView::SetYuy2Pixels(uint8_t* vmo_base, uint8_t pixel_multiplier) {
  for (uint32_t y_iter = 0; y_iter < kShapeHeight; y_iter++) {
    double y = static_cast<double>(y_iter) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth; x_iter += 2) {
      double x0 = static_cast<double>(x_iter) / kShapeWidth;
      double x1 = static_cast<double>(x_iter + 1) / kShapeWidth;
      uint8_t* two_pixels =
          &vmo_base[y_iter * stride_ + x_iter * sizeof(uint16_t)];
      two_pixels[0] = GetYValue(x0, y) * pixel_multiplier;
      two_pixels[1] = GetUValue(x0, y) * pixel_multiplier;
      two_pixels[2] = GetYValue(x1, y) * pixel_multiplier;
      two_pixels[3] = GetVValue(x0, y) * pixel_multiplier;
    }
  }
}

void YuvBaseView::SetNv12Pixels(uint8_t* vmo_base, uint8_t pixel_multiplier) {
  // Y plane
  uint8_t* y_base = vmo_base;
  for (uint32_t y_iter = 0; y_iter < kShapeHeight; y_iter++) {
    double y = static_cast<double>(y_iter) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth; x_iter++) {
      double x = static_cast<double>(x_iter) / kShapeWidth;
      y_base[y_iter * stride_ + x_iter] = GetYValue(x, y) * pixel_multiplier;
    }
  }
  // UV interleaved
  uint8_t* uv_base = y_base + kShapeHeight * stride_;
  for (uint32_t y_iter = 0; y_iter < kShapeHeight / 2; y_iter++) {
    double y = static_cast<double>(y_iter * 2) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth / 2; x_iter++) {
      double x = static_cast<double>(x_iter * 2) / kShapeWidth;
      uv_base[y_iter * stride_ + x_iter * 2] =
          GetUValue(x, y) * pixel_multiplier;
      uv_base[y_iter * stride_ + x_iter * 2 + 1] =
          GetVValue(x, y) * pixel_multiplier;
    }
  }
}

void YuvBaseView::SetYv12Pixels(uint8_t* vmo_base, uint8_t pixel_multiplier) {
  // Y plane
  uint8_t* y_base = vmo_base;
  for (uint32_t y_iter = 0; y_iter < kShapeHeight; y_iter++) {
    double y = static_cast<double>(y_iter) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth; x_iter++) {
      double x = static_cast<double>(x_iter) / kShapeWidth;
      y_base[y_iter * stride_ + x_iter] = GetYValue(x, y) * pixel_multiplier;
    }
  }
  // U and V work the same as each other, so do them together
  uint8_t* u_base =
      y_base + kShapeHeight * stride_ + kShapeHeight / 2 * stride_ / 2;
  uint8_t* v_base = y_base + kShapeHeight * stride_;
  for (uint32_t y_iter = 0; y_iter < kShapeHeight / 2; y_iter++) {
    double y = static_cast<double>(y_iter * 2) / kShapeHeight;
    for (uint32_t x_iter = 0; x_iter < kShapeWidth / 2; x_iter++) {
      double x = static_cast<double>(x_iter * 2) / kShapeWidth;
      u_base[y_iter * stride_ / 2 + x_iter] =
          GetUValue(x, y) * pixel_multiplier;
      v_base[y_iter * stride_ / 2 + x_iter] =
          GetVValue(x, y) * pixel_multiplier;
    }
  }
}

double YuvBaseView::GetYValue(double x, double y) { return x; }

double YuvBaseView::GetUValue(double x, double y) { return y; }

double YuvBaseView::GetVValue(double x, double y) { return 1 - y; }

}  // namespace yuv_to_image_pipe
