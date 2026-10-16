#include "devctl.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <share.h>
#include <string>

#include "crashreport.h"
#include "game.h"
#include "fpcam.h"
#include "zonefix.h"
#include "log.h"
#include "pacing.h"
#include "probe.h"
#include "rendersize.h"
#include "stereo.h"

namespace b2s::devctl {
namespace {

std::wstring GameDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s(exe);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// ---- screenshots ----------------------------------------------------------------

void SaveShot(IDXGISwapChain* sc, const std::string& name, int scale) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count == 1 && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const UINT w = d.Width / scale, h = d.Height / scale;
            std::wstring path = GameDir() + L"\\" + std::wstring(name.begin(), name.end()) + L".bmp";
            if (FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR)) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = LONG(w);
                ih.biHeight = -LONG(h);
                ih.biPlanes = 1;
                ih.biBitCount = 24;
                const UINT stride = (w * 3 + 3) & ~3u;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + stride * h;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                std::string row(stride, '\0');
                // Assumes an RGBA8/BGRA8 backbuffer (the game's is R8G8B8A8_UNORM).
                const bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* src = static_cast<uint8_t*>(m.pData) + size_t(y) * scale * m.RowPitch;
                    for (UINT x = 0; x < w; ++x) {
                        const uint8_t* p = src + size_t(x) * scale * 4;
                        row[x * 3 + 0] = char(rgba ? p[2] : p[0]);
                        row[x * 3 + 1] = char(p[1]);
                        row[x * 3 + 2] = char(rgba ? p[0] : p[2]);
                    }
                    fwrite(row.data(), 1, stride, f);
                }
                fclose(f);
            }
            ctx->Unmap(st, 0);
            Log("DEVCTL shot %s %ux%u (fmt %d)", name.c_str(), w, h, int(d.Format));
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
    bb->Release();
}

// ---- command queue --------------------------------------------------------------

std::deque<std::string> g_queue;
int g_waitFrames = 0;
uint64_t g_frame = 0;

void PollCommandFile() {
    const std::wstring dir = GameDir();
    const std::wstring cmd = dir + L"\\b2svr_cmd.txt";
    std::ifstream in(cmd);
    if (!in) return;
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') {
            g_queue.push_back(line);
            ++n;
        }
    }
    in.close();
    MoveFileExW(cmd.c_str(), (dir + L"\\b2svr_cmd.done").c_str(), MOVEFILE_REPLACE_EXISTING);
    Log("DEVCTL queued %d commands", n);
}

void RunLua(const std::string& file) {
    using RunFn = int (*)(const char*);
    auto run = reinterpret_cast<RunFn>(game::Addr(game::Anchor::RunLuaFile));
    if (!run) {
        Log("DEVCTL lua: runner anchor unavailable");
        return;
    }
    const int r = run(file.c_str());
    Log("DEVCTL lua %s -> %d", file.c_str(), r);
}

void Execute(const std::string& line, IDXGISwapChain* sc) {
    char verb[32] = {}, arg[256] = {};
    int num = 0;
    const int got = sscanf_s(line.c_str(), "%31s %255s %d", verb, unsigned(sizeof(verb)), arg, unsigned(sizeof(arg)),
                             &num);
    const std::string v = verb;
    if (v == "wait") {
        g_waitFrames = std::atoi(arg);
    } else if (v == "shot") {
        SaveShot(sc, arg[0] ? arg : "b2svr_shot", got >= 3 && num > 0 ? num : 4);
    } else if (v == "lua") {
        RunLua(arg);
    } else if (v == "crashreport") {
        crash::WriteTestReport();
    } else if (v == "log") {
        Log("DEVCTL mark: %s", line.c_str() + 4);
    } else if (!stereo::Command(verb, arg) && !fpcam::Command(verb, arg) && !zonefix::Command(verb, arg) && !pacing::Command(verb, arg) && !probe::Command(verb, arg) &&
               !rendersize::Command(verb, arg)) {
        Log("DEVCTL unknown command '%s'", line.c_str());
    }
}

}  // namespace

void OnFrame(IDXGISwapChain* sc) {
    ++g_frame;
    if (g_waitFrames > 0) {
        --g_waitFrames;
        return;
    }
    if (g_queue.empty()) {
        if (g_frame % 15 == 0) PollCommandFile();
        return;
    }
    const std::string line = g_queue.front();
    g_queue.pop_front();
    Execute(line, sc);
}

}  // namespace b2s::devctl
