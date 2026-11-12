// Per-title host configuration (docs/decisiones.md D-019). Header-only so the unit tests can include it.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Oot3dNativeGame {

// Keyed by the program (title) id found in the exheader that sits
// next to code.bin. A title that is not listed gets the OoT3D defaults, so the OoT3D baseline behaves exactly as before.
struct TitleProfile {
  uint64_t ProgramId;
  const char* Name;
  uint8_t SystemRegion;       // cfg:u GetRegion: 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN
  bool Oot3dLanguageModules;  // the RomFS carries OoT3D's menu/message language modules (strict detection)
  bool MemoryFillInterruptPerUnit;  // MemoryFill raises PSC0 and PSC1, one per fill unit (hardware behaviour)
  bool DeferGpuCompletions;         // PSC/PPF/P3D/DMA reach the relay queue at the next VBlank, not inside the trigger SVC (D-024)
};
inline constexpr TitleProfile kDefaultTitleProfile{0, "OoT3D (default)", 2, true, false, false};
inline constexpr TitleProfile kTitleProfiles[] = {
    {0x0004000000125500ULL, "MM3D USA v1.0", 1, false, true, true},
    {0x0004000E00125500ULL, "MM3D USA update", 1, false, true, true},
};

inline uint64_t ReadProgramId(const std::filesystem::path& codeBinPath) {
  std::ifstream exheader(codeBinPath.parent_path() / "exheader.bin", std::ios::binary);
  uint8_t bytes[8] = {};
  if (!exheader.seekg(0x200) || !exheader.read(reinterpret_cast<char*>(bytes), sizeof(bytes))) {
    return 0;
  }
  uint64_t id = 0;
  for (int index = 7; index >= 0; --index) id = (id << 8U) | bytes[index];
  return id;
}

inline const TitleProfile& ResolveTitleProfile(uint64_t programId) {
  for (const TitleProfile& profile : kTitleProfiles) {
    if (profile.ProgramId == programId) return profile;
  }
  return kDefaultTitleProfile;
}

// The profile of the running title: set once at start-up (window setup), read by the GSP / PICA emulation.
inline const TitleProfile*& ActiveTitleProfileSlot() {
  static const TitleProfile* active = &kDefaultTitleProfile;
  return active;
}
inline const TitleProfile& ActiveTitleProfile() { return *ActiveTitleProfileSlot(); }
inline void SetActiveTitleProfile(const TitleProfile& profile) { ActiveTitleProfileSlot() = &profile; }

// MemoryFill interrupt policy (docs/decisiones.md D-021). TRIAEVUM_FILL_PER_UNIT=0/1 overrides the profile (diagnostic).
inline bool MemoryFillInterruptPerUnit() {
  static const char* override = std::getenv("TRIAEVUM_FILL_PER_UNIT");
  if (override != nullptr) return std::atoi(override) != 0;
  return ActiveTitleProfile().MemoryFillInterruptPerUnit;
}

// GPU completion delivery (docs/decisiones.md D-024). TRIAEVUM_GSP_DEFER=0/1 overrides the profile (diagnostic).
inline bool DeferGpuCompletions() {
  static const char* override = std::getenv("TRIAEVUM_GSP_DEFER");
  if (override != nullptr) return std::atoi(override) != 0;
  return ActiveTitleProfile().DeferGpuCompletions;
}

} // namespace Oot3dNativeGame
