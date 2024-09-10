// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ink/strokes/internal/stroke_vertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#include "absl/algorithm/container.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "ink/geometry/internal/algorithms.h"
#include "ink/geometry/mesh_format.h"
#include "ink/geometry/mesh_packing_types.h"
#include "ink/geometry/mutable_mesh.h"
#include "ink/geometry/vec.h"

namespace ink::strokes_internal {
namespace {

using ::ink::geometry_internal::Lerp;
using ::ink::geometry_internal::LinearMap;

// LINT.IfChange(margin_encoding)
// The code below specifies how a vertex category and margin are encoded
// together inside a `float`. The implementation below currently does encoding
// and decoding linearly.

// The range of encoded values for the `margin_parameter`.
//
// Labels will take on integral values from -127 to 127, with the sign bit
// corresponding to the `StrokeVertex::Category`. We must distinguish between
// interior vertices and exterior vertices that have a zero margin. This is why
// the range of encoded outset ratios begins at 1.
constexpr std::pair<float, float> kRangeOfEncodedMarginValues = {1, 127};

}  // namespace

StrokeVertex::Label StrokeVertex::Label::WithMargin(
    float margin_fraction) const {
  if (encoded_value == 0) return *this;
  return {
      .encoded_value = DerivativeOutsetSign() *
                       std::floor(LinearMap(
                           std::clamp(margin_fraction, 0.f, kMaximumMargin),
                           {0.f, kMaximumMargin}, kRangeOfEncodedMarginValues)),
  };
}

float StrokeVertex::Label::DecodeMargin() const {
  if (encoded_value == 0) return 0;
  return LinearMap(std::abs(encoded_value), kRangeOfEncodedMarginValues,
                   {0.f, kMaximumMargin});
}
// LINT.ThenChange(
//     ../../rendering/skia/common_internal/sksl_vertex_shader_helper_functions.h:margin_encoding)

namespace {

std::optional<MeshAttributeCodingParams> GetCustomPackingParams(
    MeshFormat::Attribute attribute) {
  // We will supply custom packing parameters for an attribute if-and-only-if it
  // is one of the color-shift or vertex label attributes. These can use the
  // same parameters for every stroke, which allows shaders to use hard-coded
  // unpacking values instead of accepting more uniforms.

  // Color-shift components are each stored unpacked in the range [-1, 1]. In
  // order to accurately store 0, we only use 2^N - 2 values instead of the full
  // 2^N - 1 representable by the N packed bits.
  // LINT.IfChange(opacity_packing)
  constexpr MeshAttributeCodingParams::ComponentCodingParams
      kOpacityCodingParams8bit = {.offset = -1, .scale = 2.f / 254};
  // LINT.ThenChange(
  //     ../../rendering/skia/common_internal/sksl_vertex_shader_helper_functions.h:opacity_packing)
  // LINT.IfChange(hsl_packing)
  constexpr MeshAttributeCodingParams::ComponentCodingParams
      kHslCodingParams10bit = {.offset = -1, .scale = 2.f / 1022};
  // LINT.ThenChange(
  //     ../../rendering/skia/common_internal/sksl_vertex_shader_helper_functions.h:hsl_packing)

  // LINT.IfChange(label_packing)
  // Vertex labels are already represented with 1 byte's worth of integral
  // values, but in the range [-127, 127]. They only need to be shifted to fit
  // in [0, 255].
  constexpr MeshAttributeCodingParams::ComponentCodingParams
      kLabelCodingParams = {.offset = -128, .scale = 1};
  // LINT.ThenChange(
  //     ../../rendering/skia/common_internal/sksl_vertex_shader_helper_functions.h:label_packing)

  switch (attribute.id) {
    case MeshFormat::AttributeId::kOpacityShift:
      if (attribute.type ==
          MeshFormat::AttributeType::kFloat1PackedIn1UnsignedByte) {
        return MeshAttributeCodingParams{
            .components = {kOpacityCodingParams8bit}};
      }
      break;
    case MeshFormat::AttributeId::kColorShiftHsl:
      if (attribute.type ==
          MeshFormat::AttributeType::kFloat3PackedIn4UnsignedBytes_XYZ10) {
        return MeshAttributeCodingParams{.components = {kHslCodingParams10bit,
                                                        kHslCodingParams10bit,
                                                        kHslCodingParams10bit}};
      }
      break;
    case MeshFormat::AttributeId::kSideLabel:
    case MeshFormat::AttributeId::kForwardLabel:
      if (attribute.type ==
          MeshFormat::AttributeType::kFloat1PackedIn1UnsignedByte) {
        return MeshAttributeCodingParams{.components = {kLabelCodingParams}};
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

}  // namespace

StrokeVertex::CustomPackingArray StrokeVertex::MakeCustomPackingArray(
    const MeshFormat& mesh_format,
    absl::Span<const MeshFormat::AttributeId> skipped_attribute_ids) {
  absl::Span<const MeshFormat::Attribute> attributes = mesh_format.Attributes();
  CustomPackingArray packing_array(kMaxAttributeCount);
  CHECK_LE(attributes.size(), packing_array.MaxSize());

  size_t custom_packing_index = 0;
  for (MeshFormat::Attribute attribute : attributes) {
    if (absl::c_find_if(skipped_attribute_ids,
                        [attribute](MeshFormat::AttributeId id) {
                          return id == attribute.id;
                        }) != skipped_attribute_ids.end()) {
      continue;
    }
    packing_array[custom_packing_index] = GetCustomPackingParams(attribute);
    ++custom_packing_index;
  }
  packing_array.Resize(custom_packing_index);
  return packing_array;
}

namespace {

MeshFormat MakeValidatedFullFormat() {
  auto format = MeshFormat::Create(
      {
          {
              MeshFormat::AttributeType::kFloat2PackedIn3UnsignedBytes_XY12,
              MeshFormat::AttributeId::kPosition,
          },
          {
              MeshFormat::AttributeType::kFloat1PackedIn1UnsignedByte,
              MeshFormat::AttributeId::kOpacityShift,
          },
          {
              MeshFormat::AttributeType::kFloat3PackedIn4UnsignedBytes_XYZ10,
              MeshFormat::AttributeId::kColorShiftHsl,
          },
          {
              MeshFormat::AttributeType::kFloat2PackedIn3UnsignedBytes_XY12,
              MeshFormat::AttributeId::kSideDerivative,
          },
          {
              MeshFormat::AttributeType::kFloat1PackedIn1UnsignedByte,
              MeshFormat::AttributeId::kSideLabel,
          },
          {
              MeshFormat::AttributeType::kFloat2PackedIn3UnsignedBytes_XY12,
              MeshFormat::AttributeId::kForwardDerivative,
          },
          {
              MeshFormat::AttributeType::kFloat1PackedIn1UnsignedByte,
              MeshFormat::AttributeId::kForwardLabel,
          },
      },
      MeshFormat::IndexFormat::k32BitUnpacked16BitPacked);
  CHECK_OK(format);
  return *format;
}

const MeshFormat kFullFormat = MakeValidatedFullFormat();

}  // namespace

MeshFormat StrokeVertex::FullMeshFormat() {
  // `MeshFormat` is relatively small, so we return by value to prevent any
  // future issues in case, for example, the type stops being trivially
  // destructible.
  return kFullFormat;
}

StrokeVertex::FormatAttributeIndices StrokeVertex::FindAttributeIndices(
    const MeshFormat& format) {
  FormatAttributeIndices indices;
  absl::Span<const MeshFormat::Attribute> attributes = format.Attributes();
  for (size_t index = 0; index < attributes.size(); ++index) {
    switch (attributes[index].id) {
      case MeshFormat::AttributeId::kPosition:
        indices.position = index;
        break;
      case MeshFormat::AttributeId::kOpacityShift:
        indices.opacity_shift = index;
        break;
      case MeshFormat::AttributeId::kColorShiftHsl:
        indices.hsl_shift = index;
        break;
      case MeshFormat::AttributeId::kSideDerivative:
        indices.side_derivative = index;
        break;
      case MeshFormat::AttributeId::kSideLabel:
        indices.side_label = index;
        break;
      case MeshFormat::AttributeId::kForwardDerivative:
        indices.forward_derivative = index;
        break;
      case MeshFormat::AttributeId::kForwardLabel:
        indices.forward_label = index;
        break;
      default:
        break;
    }
  }
  return indices;
}

StrokeVertex StrokeVertex::GetFromMesh(const MutableMesh& mesh,
                                       uint32_t index) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  StrokeVertex vertex;
  std::memcpy(&vertex, &mesh.RawVertexData()[index * sizeof(StrokeVertex)],
              sizeof(StrokeVertex));
  return vertex;
}

Vec StrokeVertex::GetSideDerivativeFromMesh(const MutableMesh& mesh,
                                            uint32_t index) {
  return GetFromMesh(mesh, index).non_position_attributes.side_derivative;
}

Vec StrokeVertex::GetForwardDerivativeFromMesh(const MutableMesh& mesh,
                                               uint32_t index) {
  return GetFromMesh(mesh, index).non_position_attributes.forward_derivative;
}

StrokeVertex::Label StrokeVertex::GetSideLabelFromMesh(const MutableMesh& mesh,
                                                       uint32_t index) {
  return GetFromMesh(mesh, index).non_position_attributes.side_label;
}

StrokeVertex::Label StrokeVertex::GetForwardLabelFromMesh(
    const MutableMesh& mesh, uint32_t index) {
  return GetFromMesh(mesh, index).non_position_attributes.forward_label;
}

namespace {

// TODO: b/306149329 - Investigate memcpy-ing the entire struct instead of
// repeatedly calling `SetFloatVertexAttribute()`.
void SetNonPositionAttributes(
    MutableMesh& mesh, uint32_t index,
    const StrokeVertex::NonPositionAttributes& attributes) {
  // Clamp the opacity and HSL shifts to within their expected bounds so that
  // they can be packed with hard-coded `MeshAttributePackingParams`.
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.opacity_shift,
      {std::clamp(attributes.opacity_shift, -1.f, 1.f)});
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.hsl_shift,
      {std::clamp(attributes.hsl_shift[0], -1.f, 1.f),
       std::clamp(attributes.hsl_shift[1], -1.f, 1.f),
       std::clamp(attributes.hsl_shift[2], -1.f, 1.f)});

  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.side_derivative,
      {attributes.side_derivative.x, attributes.side_derivative.y});
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.side_label,
      {attributes.side_label.encoded_value});
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.forward_derivative,
      {attributes.forward_derivative.x, attributes.forward_derivative.y});
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.forward_label,
      {attributes.forward_label.encoded_value});
}

}  // namespace

void StrokeVertex::AppendToMesh(MutableMesh& mesh, const StrokeVertex& vertex) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.AppendVertex(vertex.position);
  SetNonPositionAttributes(mesh, mesh.VertexCount() - 1,
                           vertex.non_position_attributes);
}

void StrokeVertex::SetInMesh(MutableMesh& mesh, uint32_t index,
                             const StrokeVertex& vertex) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.SetVertexPosition(index, vertex.position);
  SetNonPositionAttributes(mesh, index, vertex.non_position_attributes);
}

void StrokeVertex::SetSideDerivativeInMesh(MutableMesh& mesh, uint32_t index,
                                           Vec derivative) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.side_derivative,
      {derivative.x, derivative.y});
}

void StrokeVertex::SetForwardDerivativeInMesh(MutableMesh& mesh, uint32_t index,
                                              Vec derivative) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.forward_derivative,
      {derivative.x, derivative.y});
}

void StrokeVertex::SetSideLabelInMesh(MutableMesh& mesh, uint32_t index,
                                      Label label) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.side_label,
      {label.encoded_value});
}

void StrokeVertex::SetForwardLabelInMesh(MutableMesh& mesh, uint32_t index,
                                         Label label) {
  DCHECK(MeshFormat::IsUnpackedEquivalent(mesh.Format(), kFullFormat));
  mesh.SetFloatVertexAttribute(
      index, StrokeVertex::kFullFormatAttributeIndices.forward_label,
      {label.encoded_value});
}

namespace {

StrokeVertex::Label LerpLabel(StrokeVertex::Label a, StrokeVertex::Label b,
                              float t) {
  // Discard any margin limits when interpolating as the margin will need to be
  // recalculated for any repositioned vertex anyway.
  a = a.WithMargin(StrokeVertex::kMaximumMargin);
  b = b.WithMargin(StrokeVertex::kMaximumMargin);

  if (a == b || t <= 0) return a;
  if (t >= 1) return b;
  return StrokeVertex::kInteriorLabel;
}

StrokeVertex::Label BarycentricLerpLabel(
    StrokeVertex::Label a, StrokeVertex::Label b, StrokeVertex::Label c,
    const std::array<float, 3> barycentric_coords) {
  if (barycentric_coords[0] == 0) return LerpLabel(b, c, barycentric_coords[2]);
  if (barycentric_coords[1] == 0) return LerpLabel(a, c, barycentric_coords[2]);
  if (barycentric_coords[2] == 0) return LerpLabel(a, b, barycentric_coords[1]);
  return StrokeVertex::kInteriorLabel;
}

}  // namespace

StrokeVertex::NonPositionAttributes Lerp(
    const StrokeVertex::NonPositionAttributes& a,
    const StrokeVertex::NonPositionAttributes& b, float t) {
  return {
      .opacity_shift = Lerp(a.opacity_shift, b.opacity_shift, t),
      .hsl_shift = {Lerp(a.hsl_shift[0], b.hsl_shift[0], t),
                    Lerp(a.hsl_shift[1], b.hsl_shift[1], t),
                    Lerp(a.hsl_shift[2], b.hsl_shift[2], t)},
      .side_label = LerpLabel(a.side_label, b.side_label, t),
      .forward_label = LerpLabel(a.forward_label, b.forward_label, t),
  };
}

StrokeVertex::NonPositionAttributes BarycentricLerp(
    const StrokeVertex::NonPositionAttributes& a,
    const StrokeVertex::NonPositionAttributes& b,
    const StrokeVertex::NonPositionAttributes& c,
    const std::array<float, 3>& t) {
  return {
      .opacity_shift = a.opacity_shift * t[0] + b.opacity_shift * t[1] +
                       c.opacity_shift * t[2],
      .hsl_shift = {a.hsl_shift[0] * t[0] + b.hsl_shift[0] * t[1] +
                        c.hsl_shift[0] * t[2],
                    a.hsl_shift[1] * t[0] + b.hsl_shift[1] * t[1] +
                        c.hsl_shift[1] * t[2],
                    a.hsl_shift[2] * t[0] + b.hsl_shift[2] * t[1] +
                        c.hsl_shift[2] * t[2]},
      .side_label =
          BarycentricLerpLabel(a.side_label, b.side_label, c.side_label, t),
      .forward_label = BarycentricLerpLabel(a.forward_label, b.forward_label,
                                            c.forward_label, t),
  };
}

}  // namespace ink::strokes_internal