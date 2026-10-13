#pragma once
// Finds live engine objects by vtable (EXP-031): scans committed private read-write memory for
// qwords equal to a class's vtable address. Slow (~1 s for 3 GB): call off the game thread only.
#include <cstdint>

namespace b2s::heapscan {

// Fills out[] with up to `max` addresses whose first qword is the vtable at static VA `vtStaticVa`.
// Returns the number found; `scannedMb` receives the amount of memory scanned.
int FindByVtable(uintptr_t vtStaticVa, void** out, int max, size_t* scannedMb = nullptr);

// Same, for up to 4 classes in one pass: hits of vtStaticVa[k] go to out[k * maxEach ...] and their
// number to counts[k]. A class whose vtable is not resolved gets 0 hits.
constexpr int kMaxClasses = 4;
void FindByVtables(const uintptr_t* vtStaticVa, int classes, void** out, int maxEach, int* counts,
                   size_t* scannedMb = nullptr);

}  // namespace b2s::heapscan
