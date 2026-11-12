// Addresses of Majora's Mask 3D that the host uses (hooks, game functions, globals), per build of the game code.
//
// mm3d-pc was made for one build of the USA v1.1 code. The v1.1.0 eShop update has the same code with small
// insertions: its functions sit a few hundred bytes later, and its .text is one page larger, so everything after
// .text (rodata, data, .bss) sits 0x1000 later. The function addresses below were found by relocating the
// original ones (tools/mm3d/relocate_recipe.py) and checking every one against an independent function
// inventory of that code; the data shift was checked on the game's item-slot table.
#pragma once

#include <cstdint>
#include <string_view>

namespace Oot3dNativeGame {

struct Mm3dBuild {
  const char* Name;
  uint32_t DataShift;  // added to every address at or after the end of .text (rodata, data, .bss)
  // Hooks (block entries).
  uint32_t FrustumPc, OrthographicPc, PauseTextureSetupPc, PauseCapturePc, CaptureDriverPc, LayoutPassPc, PlayCalcPc;
  // Game functions called by the cheats / warp.
  uint32_t ObjectFind, ObjectRequest, ObjectLoaded, ObjectFree, ActorSpawn;
  // Model draw hooks (frame interpolation identity).
  uint32_t ModelDrawEntry, ModelDrawReturn, PrimitiveSetDrawEntry;
};

// The build mm3d-pc was made for (code.bin SHA-256 6eec27b1...).
inline constexpr Mm3dBuild kMm3dBuildUsaV11{
    "USA v1.1", 0U,
    0x00122FE8U, 0x00131810U, 0x00141D44U, 0x001205E0U, 0x0011232CU, 0x001267E8U, 0x0014F550U,
    0x001F55CCU, 0x001F3F5CU, 0x001DF798U, 0x001F4FB0U, 0x0022053CU,
    0x005E9254U, 0x005EAD48U, 0x005E9504U};

// Project Restoration (a relinked build; data and .bss are unchanged). The cheats and the layout hooks are not
// used with it, so those entries repeat the v1.1 values.
inline constexpr Mm3dBuild kMm3dBuildRestoration{
    "Project Restoration", 0U,
    0x00123010U, 0x00131838U, 0x00141D88U, 0x00120608U, 0x00112358U, 0x001267E8U, 0x0014F550U,
    0x001F55CCU, 0x001F3F5CU, 0x001DF798U, 0x001F4FB0U, 0x0022053CU,
    0x005E9254U, 0x005EAD48U, 0x005E9504U};

// USA v1.1.0 eShop update merged with the base game (code.bin SHA-256 a1d1724d...).
inline constexpr Mm3dBuild kMm3dBuildEshop110{
    "USA v1.1.0 (eShop)", 0x1000U,
    0x001231B0U, 0x00131938U, 0x00141E6CU, 0x00120708U, 0x00112454U, 0x001269B0U, 0x0014F674U,
    0x001F57CCU, 0x001F415CU, 0x001DF998U, 0x001F51B0U, 0x0022073CU,
    0x005E9450U, 0x005EAF44U, 0x005E9700U};

inline const Mm3dBuild*& ActiveMm3dBuildSlot() {
  static const Mm3dBuild* active = &kMm3dBuildUsaV11;
  return active;
}
inline const Mm3dBuild& ActiveMm3dBuild() { return *ActiveMm3dBuildSlot(); }

// `variant` is the process manifest's title.variant ("" for the original build).
inline const Mm3dBuild& SelectMm3dBuild(std::string_view variant) {
  const Mm3dBuild* build = &kMm3dBuildUsaV11;
  if (variant == "restoration") build = &kMm3dBuildRestoration;
  if (variant == "eshop110") build = &kMm3dBuildEshop110;
  ActiveMm3dBuildSlot() = build;
  return *build;
}

// A global of the game (rodata, data or .bss) at its v1.1 address -> the address in the active build.
inline uint32_t Mm3dData(uint32_t v11Address) { return v11Address + ActiveMm3dBuild().DataShift; }

}  // namespace Oot3dNativeGame
