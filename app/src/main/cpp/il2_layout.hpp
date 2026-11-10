#pragma once
#include <cstdint>

/** Unity IL2CPP managed Dictionary / List layout (not game-specific). */
namespace Il2 {

constexpr uintptr_t DictEntries = 0x18;
constexpr uintptr_t DictCount = 0x20;
constexpr uintptr_t ArrMaxLen = 0x18;
constexpr uintptr_t ArrData = 0x20;
constexpr uintptr_t EntryStride = 24;
constexpr uintptr_t EntryHash = 0x00;
constexpr uintptr_t EntryKey = 0x08;
constexpr uintptr_t EntryValue = 0x10;
constexpr uintptr_t ListItems = 0x10;
constexpr uintptr_t ListSize = 0x18;

constexpr uintptr_t KlassName = 0x10;
constexpr uintptr_t StaticFieldOffs[] = {0xB8, 0xC0, 0xB0, 0xC8, 0xD0, 0xA8, 0xD8};

} // namespace Il2
