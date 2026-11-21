// First-start welcome windows: the drawing (see welcome_draw.h). Ported from the God of War VR mod's
// src/vr/welcome_draw.cpp (same owner, MIT; playbook 11.4 / GoW D-318 / D-328): the same tokens, Direct2D +
// DirectWrite into a premultiplied WIC bitmap (colour emoji), with the Beyond: Two Souls text and the VR panel's light
// blue (owner 2026-10-27, the Beyond cover) where GoW has red.
#include "welcome_draw.h"

#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>

#include "log.h"

// windowscodecs.dll's own factory entry (exported, in windowscodecs.lib; not in the SDK headers)
extern "C" HRESULT WINAPI WICCreateImagingFactory_Proxy(UINT SDKVersion, IWICImagingFactory** ppIWICImagingFactory);

namespace b2s::welcome {
uint32_t g_wOut[kWW * kWH];
RECT g_wBtn{};
namespace {
uint32_t g_wBg[kWW * kWH];  // the window's pattern, rounded corners (premultiplied)
// Direct2D / DirectWrite (render thread)
ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
IDWriteFontCollection* g_wFonts = nullptr;  // the built-in Montserrat (null: the system's Segoe UI)
IWICImagingFactory* g_wic = nullptr;
IWICBitmap* g_wBmp = nullptr;
ID2D1RenderTarget* g_wRt = nullptr;

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float Rnd(uint32_t h) { return float(h & 0xffffu) / 65535.f; }
float SegDist(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float t = fminf(fmaxf((wx * vx + wy * vy) / fmaxf(vx * vx + vy * vy, 1e-6f), 0.f), 1.f);
    const float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}
// distance (pixels) from p to the shape of `kind`, size s, centred at the origin of the stream frame (x along the stream)
float ShapeDist(int kind, float x, float y, float s) {
    const float h = 0.5f * s;
    switch (kind) {
        case 0:  // open chevron pointing along the stream
            return fminf(SegDist(x, y, -0.35f * s, -h, 0.35f * s, 0.f), SegDist(x, y, -0.35f * s, h, 0.35f * s, 0.f));
        case 1: {  // double chevron
            const float a = fminf(SegDist(x, y, -0.6f * s, -h, 0.1f * s, 0.f), SegDist(x, y, -0.6f * s, h, 0.1f * s, 0.f));
            const float b = fminf(SegDist(x, y, -0.1f * s, -h, 0.6f * s, 0.f), SegDist(x, y, -0.1f * s, h, 0.6f * s, 0.f));
            return fminf(a, b);
        }
        case 2: {  // diamond outline, long along the stream
            const float rx = 0.9f * s, ry = 0.45f * s;
            const float k = fabsf(x) / rx + fabsf(y) / ry - 1.f;
            return fabsf(k) * (rx * ry) / sqrtf(rx * rx + ry * ry);
        }
        case 3: {  // a long dash ending in a dot
            const float dash = SegDist(x, y, -1.6f * s, 0.f, 0.9f * s, 0.f);
            const float dot = sqrtf((x - 1.45f * s) * (x - 1.45f * s) + y * y) - 0.12f * s;
            return fminf(dash, fmaxf(dot, 0.f));
        }
        default: {  // three loose dots
            const float d1 = sqrtf((x + 0.5f * s) * (x + 0.5f * s) + y * y);
            const float d2 = sqrtf(x * x + (y - 0.25f * s) * (y - 0.25f * s));
            const float d3 = sqrtf((x - 0.5f * s) * (x - 0.5f * s) + y * y);
            return fmaxf(fminf(fminf(d1, d2), d3) - 0.09f * s, 0.f);
        }
    }
}

const void* FontRes(int id, DWORD* size) {
    HMODULE mod = nullptr;
    *size = 0;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&FontRes), &mod))
        return nullptr;
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
    *size = r ? SizeofResource(mod, r) : 0;
    return g ? LockResource(g) : nullptr;
}
}  // namespace

// one pixel of the V6 pattern (0x00RRGGBB): grey (42,45,48), shapes (53,56,60), streams rising 30 deg to the right
uint32_t PatternPixel(int px, int py) {
    const float bgR = 42.f, bgG = 45.f, bgB = 48.f;
    const float inkR = 53.f, inkG = 56.f, inkB = 60.f;
    const float ca = 0.8660254f, sa = 0.5f;
    const float cellU = 200.f, cellV = 92.f, stroke = 6.f;
    const float u = float(px) * ca - float(py) * sa, v = float(px) * sa + float(py) * ca;  // along / across the streams
    const int row = int(floorf(v / cellV));
    const float uo = u + Rnd(Hash(uint32_t(row) * 7919u + 13u)) * cellU;  // rows staggered
    const int col = int(floorf(uo / cellU));
    const uint32_t h = Hash(uint32_t(row) * 73856093u ^ uint32_t(col) * 19349663u);
    float cov = 0.f;
    const int kind = int(h % 6u);  // 5 = an empty cell
    if (kind < 5) {
        const float s = (kind == 3 ? 34.f : 44.f) + Rnd(h >> 8) * 16.f;
        const float cx = (float(col) + 0.5f) * cellU + (Rnd(h >> 4) - 0.5f) * 30.f;
        const float cy = (float(row) + 0.5f) * cellV + (Rnd(h >> 12) - 0.5f) * 16.f;
        const float d = ShapeDist(kind, uo - cx, v - cy, s);
        const float w = kind == 4 ? 0.f : 0.5f * stroke;
        cov = fminf(fmaxf(w + 0.75f - d, 0.f), 1.5f) / 1.5f;  // anti-aliased edge
    }
    const float r = bgR + (inkR - bgR) * cov, g = bgG + (inkG - bgG) * cov, b = bgB + (inkB - bgB) * cov;
    return uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b);
}

bool LoadPanelFonts() {
    static int state = 0;  // 1 loaded, -1 failed
    if (state) return state > 0;
    int ok = 0;
    for (int id = 3101; id <= 3103; ++id) {
        DWORD size = 0;
        const void* p = FontRes(id, &size);
        DWORD n = 0;
        if (p && AddFontMemResourceEx(const_cast<void*>(p), size, nullptr, &n) && n > 0) ++ok;
    }
    state = ok == 3 ? 1 : -1;
    return state > 0;
}

bool MakeD2D() {
    static int state = 0;  // 1 ready, -1 failed
    if (state) return state > 0;
    state = -1;
    // The WIC factory without COM: this is the game's render thread - no CoInitialize may be left on it.
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw))) ||
        FAILED(WICCreateImagingFactory_Proxy(WINCODEC_SDK_VERSION, &g_wic)) ||
        FAILED(g_wic->CreateBitmap(kWW, kWH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &g_wBmp))) {
        Log("WELCOME Direct2D / WIC not available - no welcome windows");
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g_d2d->CreateWicBitmapRenderTarget(g_wBmp, rp, &g_wRt))) {
        Log("WELCOME no Direct2D render target - no welcome windows");
        return false;
    }
    g_wRt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    // the built-in Montserrat for DirectWrite (an in-memory font set; Windows 10 1703+)
    IDWriteFactory5* f5 = nullptr;
    if (SUCCEEDED(g_dw->QueryInterface(IID_PPV_ARGS(&f5)))) {
        IDWriteInMemoryFontFileLoader* ld = nullptr;
        IDWriteFontSetBuilder1* b = nullptr;
        if (SUCCEEDED(f5->CreateInMemoryFontFileLoader(&ld)) && SUCCEEDED(f5->RegisterFontFileLoader(ld)) &&
            SUCCEEDED(f5->CreateFontSetBuilder(&b))) {
            int n = 0;
            for (int id = 3101; id <= 3103; ++id) {
                DWORD sz = 0;
                const void* p = FontRes(id, &sz);
                IDWriteFontFile* ff = nullptr;
                if (p && SUCCEEDED(ld->CreateInMemoryFontFileReference(f5, p, sz, nullptr, &ff))) {
                    if (SUCCEEDED(b->AddFontFile(ff))) ++n;
                    ff->Release();
                }
            }
            IDWriteFontSet* set = nullptr;
            IDWriteFontCollection1* c = nullptr;
            if (n == 3 && SUCCEEDED(b->CreateFontSet(&set)) && SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &c)))
                g_wFonts = c;
            if (set) set->Release();
        }
        if (b) b->Release();
        f5->Release();  // (the loader stays registered: the collection's files live in it)
    }
    Log("WELCOME drawing ready, font %s", g_wFonts ? "Montserrat (built in)" : "Segoe UI");
    // the pattern with rounded corners, premultiplied
    const float rad = 44.f;
    for (int y = 0; y < kWH; ++y)
        for (int x = 0; x < kWW; ++x) {
            const float qx = fmaxf(fabsf(float(x) + 0.5f - kWW * 0.5f) - (kWW * 0.5f - rad), 0.f);
            const float qy = fmaxf(fabsf(float(y) + 0.5f - kWH * 0.5f) - (kWH * 0.5f - rad), 0.f);
            const float a = fminf(fmaxf(rad + 0.5f - sqrtf(qx * qx + qy * qy), 0.f), 1.f);
            const uint32_t c = PatternPixel(x, y);
            const uint32_t r = uint32_t(lrintf(float((c >> 16) & 255) * a)), g = uint32_t(lrintf(float((c >> 8) & 255) * a)),
                           bl = uint32_t(lrintf(float(c & 255) * a));
            g_wBg[y * kWW + x] = uint32_t(lrintf(a * 255.f)) << 24 | r << 16 | g << 8 | bl;
        }
    state = 1;
    return true;
}

namespace {
IDWriteTextFormat* WFmt(float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(g_wFonts ? L"Montserrat" : L"Segoe UI", g_wFonts, w, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    return f;
}
// a centred text block from y across the window's text width; returns its height (pixels)
float WText(const wchar_t* s, float y, float size, DWRITE_FONT_WEIGHT w, D2D1_COLOR_F col, float spacing = 0.f,
            float width = kWW - 180.f) {
    IDWriteTextFormat* f = WFmt(size, w);
    if (!f) return 0.f;
    IDWriteTextLayout* l = nullptr;
    float h = 0.f;
    const UINT32 len = UINT32(wcslen(s));
    if (SUCCEEDED(g_dw->CreateTextLayout(s, len, f, width, 1000.f, &l))) {
        // the PlayStation button marks (U+25A0..25FF shapes, U+2715 cross) from Segoe UI Symbol at full size (the
        // automatic fallback picked a font with tiny ones)
        IDWriteFontCollection* sys = nullptr;
        for (UINT32 i = 0; i < len; ++i)
            if ((s[i] >= 0x25A0 && s[i] <= 0x25FF) || s[i] == 0x2715) {
                if (!sys) g_dw->GetSystemFontCollection(&sys);
                if (sys) l->SetFontCollection(sys, {i, 1});
                l->SetFontFamilyName(L"Segoe UI Symbol", {i, 1});
            }
        if (sys) sys->Release();
        if (spacing != 0.f) {
            IDWriteTextLayout1* l1 = nullptr;
            if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&l1)))) {
                l1->SetCharacterSpacing(0.f, spacing, 0.f, {0, len});
                l1->Release();
            }
        }
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        h = m.height;
        ID2D1SolidColorBrush* br = nullptr;
        g_wRt->CreateSolidColorBrush(col, &br);
        if (br) {
            g_wRt->DrawTextLayout({(kWW - width) * 0.5f, y}, l, br, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            br->Release();
        }
        l->Release();
    }
    f->Release();
    return h;
}
}  // namespace

void WelcomeDraw(int page, bool hover) {
    if (!MakeD2D()) return;
    {  // the pattern under everything
        WICRect r{0, 0, kWW, kWH};
        IWICBitmapLock* lk = nullptr;
        if (SUCCEEDED(g_wBmp->Lock(&r, WICBitmapLockWrite, &lk))) {
            UINT stride = 0, sz = 0;
            BYTE* p = nullptr;
            lk->GetStride(&stride);
            lk->GetDataPointer(&sz, &p);
            for (int y = 0; y < kWH && p; ++y) memcpy(p + size_t(y) * stride, g_wBg + size_t(y) * kWW, size_t(kWW) * 4);
            lk->Release();
        }
    }
    // The panel's light blue (Beyond cover, owner 2026-10-27) where GoW has red, in the same relations: the title-line
    // balls as the panel's selected-hover shade #7DB0D3, the section labels as its pointer ring #8DBEDE.
    const D2D1_COLOR_F white{1.f, 1.f, 1.f, 1.f}, body{0.92f, 0.92f, 0.94f, 1.f}, dim{0.68f, 0.68f, 0.71f, 1.f},
                       ball{125.f / 255.f, 176.f / 255.f, 211.f / 255.f, 1.f},
                       accent{141.f / 255.f, 190.f / 255.f, 222.f / 255.f, 1.f};
    g_wRt->BeginDraw();
    ID2D1SolidColorBrush* br = nullptr;
    g_wRt->CreateSolidColorBrush(white, &br);
    if (br) {
        // the window's thin edge
        br->SetOpacity(0.22f);
        g_wRt->DrawRoundedRectangle({{1.5f, 1.5f, kWW - 1.5f, kWH - 1.5f}, 43.f, 43.f}, br, 3.f);
        // the title, a line under it ending in a ball at each end
        static const wchar_t* titles[kWelPages] = {L"WELCOME", L"KO-FI SUPPORTER", L"VR CONTROLS"};
        WText(titles[page], 52.f, 58.f, DWRITE_FONT_WEIGHT_BOLD, white, 4.f);
        br->SetOpacity(0.85f);
        g_wRt->DrawLine({104.f, 150.f}, {kWW - 104.f, 150.f}, br, 4.f);
        br->SetColor(ball);
        br->SetOpacity(1.f);
        g_wRt->FillEllipse({{90.f, 150.f}, 8.f, 8.f}, br);
        g_wRt->FillEllipse({{kWW - 90.f, 150.f}, 8.f, 8.f}, br);
        // the page
        float y = 200.f;
        if (page == 0) {
            y += 85.f;  // (centred between the line and the bubble)
            y += WText(L"You're about to experience\nmy Beyond: Two Souls VR mod.\nHope you have fun! \U0001F60E", y, 42.f,
                       DWRITE_FONT_WEIGHT_SEMI_BOLD, body);
            y += 40.f;
            WText(L"Note: the mod is still a work in progress.", y, 32.f, DWRITE_FONT_WEIGHT_NORMAL, dim);
        } else if (page == 1) {
            y += 50.f;
            y += WText(L"A special shoutout to the person who contributed\nto help me make more great VR mods.\n"
                       L"Thank you very much! \U0001F64F",
                       y, 38.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, body);
            y += 40.f;
            WText(L"ZarDomain \U0001F4AA", y, 84.f, DWRITE_FONT_WEIGHT_BOLD, white);  // owner 2026-11-15: "ZarDomain"
        } else {
            // Owner 2026-11-15: no game-specific window before this one; the examples line names this port's own
            // settings ("tinker in VR settings (Aiden possessions etc.)").
            y += WText(L"In the game's Options menu you'll find the VR panel,\nwhere you can tinker with lots of settings\n"
                       L"(first person, Aiden possessions and more).",
                       y, 33.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, body);
            y += 30.f;
            y += WText(L"FIRST PERSON SHORTCUT", y, 26.f, DWRITE_FONT_WEIGHT_BOLD, accent, 2.f);
            y += 4.f;
            y += WText(L"Press B (Quest / Index) or \x25CB (PS VR2) next to your head", y, 32.f, DWRITE_FONT_WEIGHT_NORMAL,
                       body);
            y += 22.f;
            y += WText(L"OPTIONS SHORTCUT", y, 26.f, DWRITE_FONT_WEIGHT_BOLD, accent, 2.f);
            y += 4.f;
            WText(L"Press all four face buttons together\nA B X Y (Quest / Index) or \x2715 \x25CB \x25B3 \x25A1 (PS VR2)", y,
                  32.f, DWRITE_FONT_WEIGHT_NORMAL, body);
        }
        // the bubble: transparent fill, white stroke (pointed at: a light fill)
        const float bw = 420.f, bh = 92.f, bx = (kWW - bw) * 0.5f, by = kWH - 178.f;
        g_wBtn = {LONG(bx), LONG(by), LONG(bx + bw), LONG(by + bh)};
        const D2D1_ROUNDED_RECT pill{{bx, by, bx + bw, by + bh}, bh * 0.5f, bh * 0.5f};
        br->SetColor(white);
        if (hover) {
            br->SetOpacity(0.16f);
            g_wRt->FillRoundedRectangle(pill, br);
        }
        br->SetOpacity(1.f);
        g_wRt->DrawRoundedRectangle(pill, br, hover ? 5.f : 4.f);
        WText(page == kWelPages - 1 ? L"START" : L"CONTINUE", by + (bh - 46.f) * 0.5f - 2.f, 36.f, DWRITE_FONT_WEIGHT_BOLD,
              white, 4.f, bw);
        // which window (the dots centred under the bubble)
        for (int i = 0; i < kWelPages; ++i) {
            br->SetOpacity(i == page ? 1.f : 0.3f);
            g_wRt->FillEllipse({{kWW * 0.5f + (float(i) - 0.5f * float(kWelPages - 1)) * 30.f, kWH - 50.f}, 7.f, 7.f}, br);
        }
        br->Release();
    }
    const HRESULT hr = g_wRt->EndDraw();
    if (FAILED(hr)) Log("WELCOME drawing failed 0x%08lx", static_cast<unsigned long>(hr));
    WICRect r{0, 0, kWW, kWH};
    IWICBitmapLock* lk = nullptr;
    if (SUCCEEDED(g_wBmp->Lock(&r, WICBitmapLockRead, &lk))) {
        UINT stride = 0, sz = 0;
        BYTE* p = nullptr;
        lk->GetStride(&stride);
        lk->GetDataPointer(&sz, &p);
        for (int y = 0; y < kWH && p; ++y) memcpy(g_wOut + size_t(y) * kWW, p + size_t(y) * stride, size_t(kWW) * 4);
        lk->Release();
    }
}

}  // namespace b2s::welcome
