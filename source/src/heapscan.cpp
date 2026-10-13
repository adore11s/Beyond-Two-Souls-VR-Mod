#include "heapscan.h"

#include <windows.h>

#include "game.h"

namespace b2s::heapscan {
namespace {

// SEH-guarded: regions can be decommitted while we scan.
void ScanRegion(const uint8_t* base, size_t size, const uintptr_t* vt, int classes, void** out, int maxEach,
                int* counts) {
    __try {
        const auto* q = reinterpret_cast<const uintptr_t*>(base);
        for (size_t i = 0; i < size / 8; ++i)
            for (int k = 0; k < classes; ++k)
                if (q[i] == vt[k] && counts[k] < maxEach) out[k * maxEach + counts[k]++] = const_cast<uintptr_t*>(q + i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

void FindByVtables(const uintptr_t* vtStaticVa, int classes, void** out, int maxEach, int* counts,
                   size_t* scannedMb) {
    if (classes > kMaxClasses) classes = kMaxClasses;
    uintptr_t vt[kMaxClasses]{};
    bool any = false;
    for (int k = 0; k < classes; ++k) {
        counts[k] = 0;
        vt[k] = game::Va(vtStaticVa[k]);
        // An unresolved class matches nothing: its slot holds a value no qword in a heap equals.
        if (!vt[k]) vt[k] = 1;
        else any = true;
    }
    size_t scanned = 0;
    if (any) {
        MEMORY_BASIC_INFORMATION mbi{};
        for (uintptr_t a = 0x10000; a < 0x7ff000000000ull; a += mbi.RegionSize) {
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
                ScanRegion(static_cast<const uint8_t*>(mbi.BaseAddress), mbi.RegionSize, vt, classes, out, maxEach,
                           counts);
                scanned += mbi.RegionSize;
            }
        }
    }
    if (scannedMb) *scannedMb = scanned >> 20;
}

int FindByVtable(uintptr_t vtStaticVa, void** out, int max, size_t* scannedMb) {
    int count = 0;
    FindByVtables(&vtStaticVa, 1, out, max, &count, scannedMb);
    return count;
}

}  // namespace b2s::heapscan
