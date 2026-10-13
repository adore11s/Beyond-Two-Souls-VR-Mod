#include "../src/steam_shadow.h"
#include <cstdio>

int main() {
    using b2s::vrinput::detail::ShadowTable;
    int failed=0, checks=0;
    auto check=[&](bool ok,const char* name) {
        ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n",name); }
    };
    int native=0, rtti=0, overrides[9]{};
    void* shared[65]{};
    shared[0]=&rtti;
    for (size_t i=1;i<65;++i) shared[i]=&native;
    void* detours[9]{};
    for (size_t i=0;i<9;++i) detours[i]=&overrides[i];
    // All original methods can alias the same short no-op. Per-interface slots
    // must still accept different handlers without modifying another interface.
    void** interfaceA=shared+1;
    void** interfaceB=shared+1;
    ShadowTable shadow;
    shadow.Build(interfaceA,detours);
    interfaceA=shadow.Table();
    check(interfaceA[3]==detours[0] && interfaceA[5]==detours[1],"aliased methods get distinct handlers");
    check(interfaceB[3]==&native && interfaceB[5]==&native,"other interface unchanged");
    check(interfaceA[-1]==&rtti,"RTTI preserved");
    check(interfaceA[0]==&native && interfaceA[63]==&native,"unrelated methods preserved");
    constexpr size_t slots[]{3,5,11,12,14,15,20,23,26};
    for (size_t i=0;i<9;++i) check(interfaceA[slots[i]]==detours[i],"all Steam input slots routed");
    std::printf("STEAM SHADOW CHECKS: %d passed, %d failed\n",checks-failed,failed);
    return failed ? 1 : 0;
}
