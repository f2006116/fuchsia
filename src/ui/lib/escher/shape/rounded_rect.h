// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_SHAPE_ROUNDED_RECT_H_
#define SRC_UI_LIB_ESCHER_SHAPE_ROUNDED_RECT_H_

#include <cstdint>
#include <utility>

#include "src/ui/lib/escher/geometry/types.h"

namespace escher {

struct MeshSpec;

// Specify a rounded-rect that is centered at (0,0).
struct RoundedRectSpec {
  // Note: radii are in clockwise order, starting from top-left.
  RoundedRectSpec(float width, float height, float top_left_radius,
                  float top_right_radius, float bottom_right_radius,
                  float bottom_left_radius);
  // Set all values to 0.
  RoundedRectSpec();

  float width;
  float height;
  float top_left_radius;
  float top_right_radius;
  float bottom_right_radius;
  float bottom_left_radius;

  bool ContainsPoint(vec2 point) const;

  // Make RoundedRectSpec lerpable.
  RoundedRectSpec operator*(float t) const;
  RoundedRectSpec operator+(const RoundedRectSpec& other) const;
};

// Return the number of vertices and indices that are required to tessellate the
// specified rounded-rect.  The first element of the pair is the vertex count,
// and the second element is the index count.
std::pair<uint32_t, uint32_t> GetRoundedRectMeshVertexAndIndexCounts(
    const RoundedRectSpec& spec);

void GenerateRoundedRectIndices(const RoundedRectSpec& spec,
                                const MeshSpec& mesh_spec, void* indices_out,
                                uint32_t max_bytes);

void GenerateRoundedRectVertices(const RoundedRectSpec& spec,
                                 const MeshSpec& mesh_spec, void* vertices_out,
                                 uint32_t max_bytes);

void GenerateRoundedRectVertices(const RoundedRectSpec& spec,
                                 const MeshSpec& mesh_spec,
                                 void* primary_attributes_out,
                                 uint32_t max_primary_attribute_bytes,
                                 void* secondary_attributes_out,
                                 uint32_t max_secondary_attribute_bytes);

// Inline function definitions.

// Provide a corresponding function to RoundedRectSpec::operator*, so that
// scalar multiplication is commutative.
inline RoundedRectSpec operator*(float t, const RoundedRectSpec& spec) {
  return spec * t;
}

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_SHAPE_ROUNDED_RECT_H_
