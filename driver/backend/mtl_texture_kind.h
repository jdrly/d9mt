#pragma once

#include <cstdint>

/* The texture kind a shader declares at a sampler (dcl_2d, dcl_cube, dcl_volume) against the
 * texture bound there (#150). Metal requires the bound texture's type to be the declared one: a 2D
 * texture where the shader expects a cube is an invalid binding (Metal's shader validation:
 * "Invalid texture type MTLTextureType2D bound to shader, expected MTLTextureTypeCube"), and the
 * GPU's read through it is undefined. D3D9 leaves such a draw undefined too, so a texture of
 * another kind is bound as the null texture of the declared kind: it reads zero, as an unbound
 * sampler does. */
namespace dxvk::backend::texkind {

  // The declared kinds (MtlShaderFunction::AbWord::dim), and every other Metal texture type.
  constexpr uint8_t k2D    = 0u;
  constexpr uint8_t kCube  = 1u;
  constexpr uint8_t k3D    = 2u;
  constexpr uint8_t kOther = 0xFEu;

  // The kind of a Metal texture type (MTLTextureType / WMTTextureType numbering: 2 2D, 5 cube,
  // 7 3D). Arrays and multisample textures are no kind a D3D9 sampler declares.
  constexpr uint8_t ofMetalType(uint32_t type) {
    return type == 2u ? k2D : type == 5u ? kCube : type == 7u ? k3D : kOther;
  }

  struct NullIds {
    uint64_t tex2D;
    uint64_t cube;
    uint64_t tex3D;
  };

  // The null texture of declared kind dim.
  constexpr uint64_t nullOf(uint8_t dim, const NullIds& n) {
    return dim == kCube ? n.cube : dim == k3D ? n.tex3D : n.tex2D;
  }

  // A texture of that kind may be bound where the shader declares kind dim.
  constexpr bool matches(uint8_t dim, uint8_t kind) {
    return kind == dim;
  }

  constexpr const char* name(uint8_t kind) {
    return kind == k2D ? "2D" : kind == kCube ? "cube" : kind == k3D ? "3D" : "array or multisample";
  }

}
