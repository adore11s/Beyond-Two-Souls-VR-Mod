// Privacy desk test for src/log.cpp: what players send (b2svr.log, crash reports) must not name them.
#include <windows.h>

#include <share.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/log.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    }
}

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    // A pretend player: profile C:\Users\Jane Doe, PC JANES-PC, game inside the profile.
    std::wstring profile = L"C:\\Users\\Jane Doe";
    SetEnvironmentVariableW(L"USERPROFILE", profile.c_str());
    SetEnvironmentVariableW(L"USERNAME", L"Jane Doe");
    SetEnvironmentVariableW(L"COMPUTERNAME", L"JANES-PC");
    const std::wstring game = std::wstring(temp) + L"b2svr_log_test\\Jane Doe Games\\Beyond";
    CreateDirectoryW((std::wstring(temp) + L"b2svr_log_test").c_str(), nullptr);
    CreateDirectoryW((std::wstring(temp) + L"b2svr_log_test\\Jane Doe Games").c_str(), nullptr);
    CreateDirectoryW(game.c_str(), nullptr);
    b2s::LogOpen(game.c_str());

    char gameUtf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, game.c_str(), -1, gameUtf8, sizeof(gameUtf8), nullptr, nullptr);
    b2s::Log("dll=%s\\DINPUT8.dll", gameUtf8);
    b2s::Log("runtime json C:\\Users\\Jane Doe\\AppData\\Local\\VirtualDesktop\\vd.json");
    b2s::Log("other drive D:\\Stuff\\Jane Doe\\x.txt and host JANES-PC");
    b2s::Log("an ordinary sentence that says Jane Doe is not a path");
    b2s::LogFlush();

    char line[512];
    strcpy_s(line, "C:\\USERS\\JANE DOE\\Desktop");
    b2s::LogScrub(line, sizeof(line));
    Check(std::strcmp(line, "<user profile>\\Desktop") == 0, "profile path, other case");

    // shared read: the logger still has the file open for writing
    FILE* f = _wfsopen((game + L"\\b2svr_logs\\b2svr.log").c_str(), L"rb", _SH_DENYNO);
    Check(f != nullptr, "log written");
    std::string text;
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    Check(text.find("<game folder>\\DINPUT8.dll") != std::string::npos, "game folder replaced");
    Check(text.find("<user profile>\\AppData") != std::string::npos, "profile replaced");
    Check(text.find("D:\\Stuff\\<user>\\x.txt") != std::string::npos, "user name as a path part replaced");
    Check(text.find("<pc>") != std::string::npos, "computer name replaced");
    Check(text.find("JANES-PC") == std::string::npos, "no computer name left");
    Check(text.find("Jane Doe Games") == std::string::npos && text.find("Users\\Jane Doe") == std::string::npos &&
              text.find("\\Jane Doe\\") == std::string::npos,
          "no user name in any path");
    Check(text.find("says Jane Doe is not a path") != std::string::npos, "ordinary words left alone");
    if (!g_fail) std::printf("log_tests: all passed\n");
    return g_fail ? 1 : 0;
}
