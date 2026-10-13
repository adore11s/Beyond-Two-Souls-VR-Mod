#pragma once
// Build-independent game addresses. Every address the mod uses is recorded for the analysed build
// together with a way to find it again in any other build of the game (another store, an update):
//   Code     a byte signature at the address (relocatable fields wildcarded), unique in .text;
//   Data     a signature around an instruction that references the global RIP-relatively;
//   Derived  a global at a recorded distance from a Data entry;
//   Vtable   the class's RTTI name, resolved through the image's RTTI records;
//   Slot     a virtual function not unique by its own bytes: slot N of a class's vtable (RTTI).
// On the analysed build every recorded address is checked against its signature in place
// ("verified"); on any other build each entry is searched for: verified, moved, missing or
// ambiguous. Nothing is used unless it was found exactly once.
#include <cstdint>

namespace b2s::sigdb {

enum class SigKind : uint8_t { Code, Data, Derived, Vtable, Slot };
enum class Status : uint8_t { Unresolved, Verified, Moved, Missing, Ambiguous };

struct Summary {
    int total = 0, verified = 0, moved = 0, missing = 0, ambiguous = 0;
};

// Resolves every entry. `knownBuild`: the .text hash is the analysed build's, so recorded
// addresses are used as they are (signatures are still checked and disagreements logged).
// Safe to call again (e.g. while a SteamStub build is still decrypting .text).
Summary Resolve(bool knownBuild, bool log);

// Runtime address of a recorded VA of the analysed build (0x14xxxxxxx), or 0 when that entry is
// missing / ambiguous in this build, or was never recorded.
uintptr_t Va(uint64_t recordedVa);
Status StatusOf(uint64_t recordedVa);

}  // namespace b2s::sigdb
