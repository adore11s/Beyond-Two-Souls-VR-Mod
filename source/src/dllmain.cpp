// dinput8.dll proxy: forwards DirectInput to the system DLL and starts B2SVR.
#include <windows.h>
#include <bcrypt.h>
#include <unknwn.h>

#include <atomic>
#include <cstring>
#include <string>

#include "crashreport.h"
#include "game.h"
#include "log.h"
#include "probe.h"

#define B2SVR_VERSION "0.17.12"

namespace {

HMODULE g_self = nullptr;
HMODULE g_realDinput = nullptr;

HMODULE RealDinput() {
    if (!g_realDinput) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(path);
    }
    return g_realDinput;
}

template <typename Fn>
Fn RealProc(const char* name) {
    HMODULE m = RealDinput();
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, name)) : nullptr;
}

std::wstring DirOf(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), int(s.size()), nullptr, nullptr);
    s.pop_back();
    return s;
}

// Hashes the mapped .text section. File reads of the EXE are not trustworthy: Steam
// emulators redirect them, and SteamStub builds decrypt .text only at startup.
std::string Sha256Text() {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 6) != 0) continue;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
        BCryptHashData(hash, base + sec->VirtualAddress, sec->Misc.VirtualSize, 0);
        UCHAR digest[32];
        BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        std::string hex;
        char tmp[3];
        for (UCHAR b : digest) {
            sprintf_s(tmp, "%02x", b);
            hex += tmp;
        }
        return hex;
    }
    return "no-.text";
}

DWORD WINAPI InitThread(LPVOID) {
    wchar_t exePath[MAX_PATH], dllPath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    GetModuleFileNameW(g_self, dllPath, MAX_PATH);
    const std::wstring gameDir = DirOf(exePath);
    b2s::LogOpen(gameDir.c_str());

    // RE-03: the first line alone must prove which bytes ran in which process.
    b2s::Log("B2SVR " B2SVR_VERSION " built " __DATE__ " " __TIME__ " dll=%s", Narrow(dllPath).c_str());
    b2s::Log("pid=%lu exe=%s base=%p arch=x64", GetCurrentProcessId(), Narrow(exePath).c_str(),
             GetModuleHandleW(nullptr));
    b2s::crash::Install(B2SVR_VERSION);
    // SteamStub builds decrypt .text after our DLL loads: retry until the core addresses resolve
    // (the analysed build by its hash, any other build by signature), at most 60 s.
    std::string textHash;
    bool core = false;
    const ULONGLONG start = GetTickCount64();
    int attempts = 0;
    while (!core && GetTickCount64() - start < 60000) {
        textHash = Sha256Text();
        core = b2s::game::ResolveAll(textHash.c_str(), false);
        ++attempts;
        if (!core) Sleep(250);
    }
    b2s::Log("game addresses: %d attempt(s), %.1f s", attempts, (GetTickCount64() - start) / 1000.0);
    b2s::Log(".text sha256=%s", textHash.c_str());
    core = b2s::game::ResolveAll(textHash.c_str(), true);
    constexpr int kAll = int(b2s::game::Anchor::Count);
    const int resolved = b2s::game::ResolvedAnchors();
    b2s::Log("anchors resolved %d/%d", resolved, kAll);
    b2s::crash::InstallLate(textHash.c_str(), core);
    if (!core) {
        b2s::Log("FAIL-CLOSED: core game addresses not found in this build; mod disabled, game runs vanilla");
        return 0;
    }
    b2s::probe::Install();
    return 0;
}

// The game denies PROCESS_TERMINATE and can ignore WM_CLOSE while unfocused, so dev
// tooling (tools/close_game.ps1) signals this event and the process ends itself.
DWORD WINAPI QuitWatchThread(LPVOID) {
    // Per-process, auto-reset: a stale signalled event from a closed instance (a script still
    // holding its handle) must never terminate the next launch.
    wchar_t name[64];
    swprintf_s(name, L"Local\\B2SVR_Quit_%lu", GetCurrentProcessId());
    HANDLE quit = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!quit) return 0;
    WaitForSingleObject(quit, INFINITE);
    b2s::Log("quit event received; terminating");
    b2s::LogFlush();
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, IUnknown* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = RealProc<Fn>("DirectInput8Create");
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = RealProc<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn = RealProc<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        // Keep work off the loader lock.
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
        if (HANDLE t = CreateThread(nullptr, 0, QuitWatchThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
