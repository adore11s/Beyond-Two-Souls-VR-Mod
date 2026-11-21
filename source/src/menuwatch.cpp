// The VR panel in the main menu's Options (menuwatch.h, playbook 10.6). Read-only: nothing of the game is written.
#include "menuwatch.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "game.h"
#include "log.h"

namespace b2s::menuwatch {
namespace {

constexpr uint64_t kControllersVa = 0x141ca6d50;  // g_activeControllers: array +0, count +8 (uint32), sigdb
// LIVE 1.0.3 (the owner in the main menu's Options, read with the mod's peek): SEQUENCE_CONTROLLER_PLAY +0x110 = the
// KCOM handle of the SEQUENCE it plays: handle +0 class (4288 = SEQUENCE), +4 the object id (the script's NewOS id:
// 6345 = MENU_GAME_sOptions), +0x18 the name holder (*holder = "MENU_GAME_sOptions"; game::ObjectName's rule). Live at
// the same time: MENU_GAME_sSmooth (6335) and MENU_GAME_s26Epilogues (5328), the menu's background sequences.
constexpr size_t kSeqHandle = 0x110;
constexpr uint32_t kSequenceClass = 4288, kOptionsId = 6345;
constexpr char kPagePrefix[] = "MENU_GAME_s";

std::atomic<bool> g_open{false};
char g_options[64] = "MENU_GAME_sOptions";
bool g_loaded = false;

template <typename T>
bool Rd(const void* base, size_t off, T* out) {
    __try {
        std::memcpy(out, static_cast<const uint8_t*>(base) + off, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The object's RTTI class is SEQUENCE_CONTROLLER_PLAY (".?AVSEQUENCE_CONTROLLER_PLAY@SINT@QDT@@"); cached per vtable.
bool IsSequencePlay(void* obj) {
    static uintptr_t yes[8] = {}, no[64] = {};
    static int nYes = 0, nNo = 0;
    uintptr_t vt = 0;
    if (!obj || !Rd(obj, 0, &vt) || !vt) return false;
    for (int i = 0; i < nYes; ++i)
        if (yes[i] == vt) return true;
    for (int i = 0; i < nNo; ++i)
        if (no[i] == vt) return false;
    bool match = false;
    __try {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* col = *reinterpret_cast<const uint32_t* const*>(vt - 8);
        const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
        match = std::strcmp(name, ".?AVSEQUENCE_CONTROLLER_PLAY@SINT@QDT@@") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        match = false;
    }
    if (match && nYes < 8) yes[nYes++] = vt;
    else if (!match && nNo < 64) no[nNo++] = vt;
    return match;
}

// The sequence this controller plays: its name into `name` and its object id; false when it has no live handle.
bool SequenceOf(void* c, char* name, size_t cap, uint32_t* id) {
    name[0] = 0;
    uintptr_t h = 0;
    uint32_t cls = 0;
    if (!Rd(c, kSeqHandle, &h) || h < 0x10000 || !Rd(reinterpret_cast<void*>(h), 0, &cls) || cls != kSequenceClass ||
        !Rd(reinterpret_cast<void*>(h), 4, id))
        return false;
    uintptr_t holder = 0, str = 0;
    if (!Rd(reinterpret_cast<void*>(h), 0x18, &holder) || holder < 0x10000 || !Rd(reinterpret_cast<void*>(holder), 0, &str) ||
        str < 0x10000)
        return true;  // the id alone
    __try {
        const char* p = reinterpret_cast<const char*>(str);
        size_t i = 0;
        for (; i + 1 < cap && p[i] >= 32 && p[i] < 127; ++i) name[i] = p[i];
        name[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        name[0] = 0;
    }
    return true;
}

void LoadSetting() {
    g_loaded = true;
    wchar_t path[MAX_PATH]{}, value[64]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring ini = path;
    ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"b2svr.ini";
    GetPrivateProfileStringW(L"b2svr", L"options_menu_page", L"MENU_GAME_sOptions", value, 64, ini.c_str());
    size_t i = 0;
    for (; i + 1 < sizeof(g_options) && value[i]; ++i) g_options[i] = char(value[i]);
    g_options[i] = 0;
    Log("MENU the VR panel shows with the main menu's Options page (%s; b2svr.ini options_menu_page)", g_options);
}

}  // namespace

void Frame(bool menuScene) {
    if (!g_loaded) LoadSetting();
    static ULONGLONG at = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - at < 100) return;  // ten times a second is plenty for a menu page
    at = now;
    bool open = false;
    char page[64] = "";
    if (menuScene) {
        const uintptr_t g = game::Va(kControllersVa);
        void** arr = nullptr;
        uint32_t total = 0;
        if (g && Rd(reinterpret_cast<void*>(g), 0, &arr) && Rd(reinterpret_cast<void*>(g), 8, &total) && arr &&
            total < 4096) {
            for (uint32_t i = 0; i < total; ++i) {
                void* c = nullptr;
                char p[64];
                uint32_t id = 0;
                if (!Rd(arr, i * 8u, &c) || !c || !IsSequencePlay(c) || !SequenceOf(c, p, sizeof(p), &id)) continue;
                const bool isOptions = p[0] ? std::strcmp(p, g_options) == 0 : id == kOptionsId;
                // the page shown: the menu's page sequences (MENU_GAME_s<Page>); the background ones stay running too
                if (isOptions || (!page[0] && std::strncmp(p, kPagePrefix, sizeof(kPagePrefix) - 1) == 0 &&
                                  std::strcmp(p, "MENU_GAME_sSmooth") != 0 && std::strcmp(p, "MENU_GAME_s26Epilogues") != 0))
                    strcpy_s(page, p[0] ? p : "?");
                if (isOptions) open = true;
            }
        }
    }
    static char lastPage[64] = "?";
    if (menuScene && std::strcmp(page, lastPage) != 0) {
        static int logs = 0;
        if (logs++ < 60)
            Log("MENU main menu page %s: %s", page[0] ? page : "(none)", open ? "Options - VR panel shown" : "not Options");
        strcpy_s(lastPage, page);
    }
    if (!menuScene) strcpy_s(lastPage, "?");
    if (open != g_open.exchange(open)) Log("MENU main menu Options %s", open ? "open: VR panel beside it" : "closed");
}

bool OptionsOpen() { return g_open.load(std::memory_order_relaxed); }

}  // namespace b2s::menuwatch
