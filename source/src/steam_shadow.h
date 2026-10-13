#pragma once
#include <array>
#include <cstddef>

namespace b2s::vrinput::detail {
// ISteamController's game-used slots fit in this bounded table. Preserve the
// remaining slots and the preceding RTTI pointer exactly as published by Steam.
struct ShadowTable {
    static constexpr size_t kSlots=64;
    static constexpr size_t kOverrides=9;
    std::array<void*,kSlots+1> words{};

    void Build(void* const* original, void* const* detours) {
        words[0]=original[-1];
        for (size_t i=0;i<kSlots;++i) words[i+1]=original[i];
        constexpr size_t slots[]{3,5,11,12,14,15,20,23,26};
        for (size_t i=0;i<kOverrides;++i) words[slots[i]+1]=detours[i];
    }
    void** Table() { return words.data()+1; }
};
} // namespace b2s::vrinput::detail
