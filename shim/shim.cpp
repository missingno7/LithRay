// lithray.ren: the 32-bit LithTech 1 renderer plugin. It implements the engine ABI (3 exports, 34 slots), copies
// everything the engine exposes into bridge commands, and never draws. See docs/ARCHITECTURE.md.
#include "shim.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>

#ifndef _M_IX86
#error lithray.ren is the 32-bit side of the bridge
#endif

LT1RenderStruct *g_rs;

static HWND g_window;
static uint32_t g_modeW, g_modeH;
static bool g_in3D, g_hadScene, g_background;

float CVar(const char *name, float fallback) {
    void *h = g_rs->GetConsoleVar((char *)name);
    return h ? g_rs->GetVarValueFloat(h) : fallback;
}

// ------------------------------------------------------------------ import patches on CLIENT.EXE
static void **PatchImport(const char *dll, const char *fn, void *replacement, void **original) {
    uint8_t *image = (uint8_t *)GetModuleHandleA(nullptr);
    auto *nt = (IMAGE_NT_HEADERS32 *)(image + ((IMAGE_DOS_HEADER *)image)->e_lfanew);
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto *d = (IMAGE_IMPORT_DESCRIPTOR *)(image + dir.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char *)(image + d->Name), dll)) continue;
        uint32_t *names = (uint32_t *)(image + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void **slots = (void **)(image + d->FirstThunk);
        for (size_t i = 0; names[i]; i++) {
            if (names[i] & 0x80000000u) continue;
            if (strcmp((const char *)(image + names[i] + 2), fn)) continue;
            DWORD old;
            if (!VirtualProtect(&slots[i], sizeof(void *), PAGE_READWRITE, &old)) return nullptr;
            *original = slots[i];
            slots[i] = replacement;
            VirtualProtect(&slots[i], sizeof(void *), old, &old);
            return &slots[i];
        }
    }
    return nullptr;
}
static void RestoreImport(void **&slot, void *original) {
    if (!slot) return;
    DWORD old;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = original;
        VirtualProtect(slot, sizeof(void *), old, &old);
    }
    slot = nullptr;
}

// The engine recentres the cursor every frame, focused or not. Without focus that must do nothing.
static void **g_cursorSlot; static void *g_cursorOrig; static bool g_cursorCaptured;
static BOOL WINAPI GuardedSetCursorPos(int x, int y) {
    if (!g_cursorCaptured) return TRUE;
    return ((BOOL(WINAPI *)(int, int))g_cursorOrig)(x, y);
}
static void UpdateCursorCapture() {
    bool focused = g_window && GetForegroundWindow() == g_window;
    if (focused != g_cursorCaptured) { ShowCursor(focused ? FALSE : TRUE); g_cursorCaptured = focused; }
}

// Mouse look reads GetTickCount (15.6 ms steps): give the engine a 1 ms clock instead.
static void **g_tickSlot; static void *g_tickOrig; static DWORD g_tickOffset;
static DWORD WINAPI FineTickCount() { return timeGetTime() + g_tickOffset; }

// The engine tears the renderer down and stops the client on WM_ACTIVATEAPP(FALSE). For unattended test runs
// (lr_Background 1) that message is kept from it. Installed only then: forwarding focus loss would let
// r_TermRender unload this DLL underneath the forwarding call.
static WNDPROC g_engineProc; static HWND g_filteredWindow;
static LRESULT CALLBACK FocusFilterProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_ACTIVATEAPP && !wp) return 0;
    return CallWindowProcA(g_engineProc, w, m, wp, lp);
}
static void RemoveFocusFilter() {
    if (!g_engineProc) return;
    SetWindowLongA(g_filteredWindow, GWL_WNDPROC, (LONG)g_engineProc);
    g_engineProc = nullptr;
}

// Server.dll and CLIENT.EXE floor the frame time at 0.01 s, so the game runs fast above 100 fps. Blood II 2.1
// only: both instructions are verified to reference the constant before 0.001 is written over it.
// Result: 1 applied (or already applied), 0 this is not the expected build, 2 the module is not loaded.
static int PatchFrameTimeFloor(const char *module, uint32_t constRva, const uint32_t operandRvas[2]) {
    uint8_t *base = (uint8_t *)GetModuleHandleA(module);
    if (!base) return 2;
    float *value = (float *)(base + constRva);
    __try {
        for (int i = 0; i < 2; i++)
            if (*(uint32_t *)(base + operandRvas[i]) != (uint32_t)(uintptr_t)value) return 0;
        if (*value == 0.001f) return 1;
        if (*value != 0.01f) return 0;
        DWORD old;
        if (!VirtualProtect(value, 4, PAGE_READWRITE, &old)) return 0;
        *value = 0.001f;
        VirtualProtect(value, 4, old, &old);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool g_speedFixed, g_speedWanted;
// The server code exists twice: in CLIENT.EXE (single player runs it in-process) and in Server.dll, which is only
// loaded for a dedicated / remote setup and may appear later. A module that is not loaded is not a failure.
static void TryGameSpeedFix(bool log) {
    if (!g_speedWanted) return;
    static const uint32_t clientOps[2] = {0x564a7, 0x564c4}, serverOps[2] = {0x26df1, 0x26e07};
    int a = PatchFrameTimeFloor(nullptr, 0x80068, clientOps), b = PatchFrameTimeFloor("server.dll", 0x42ba8, serverOps);
    bool ok = a == 1 && b != 0;
    if (log || ok != g_speedFixed) bridge::Log("game speed fix: client %d server %d (1 applied, 2 not loaded, 0 unexpected build)%s", a, b, ok ? "" : ": capped at 100 fps");
    g_speedFixed = ok;
}

// ------------------------------------------------------------------ console options
static const char *kOptions[][2] = {
    {"lr_RT", "1"},            // ray-traced lighting (0 = raster baseline)
    {"lr_RTShadows", "1"}, {"lr_RTReflections", "1"}, {"lr_RTAO", "1"},
    {"lr_RTGI", "1"},          // 1: lamps + one indirect bounce replace the lightmaps; 0: lightmaps with ray-traced shadows
    {"lr_GIDirect", "0.9"}, {"lr_GIIndirect", "1"}, {"lr_GIHistory", "0.9"}, {"lr_GIEmissive", "1"},
    {"lr_Look", "1"},          // 1: modern look (linear HDR lighting, real falloff, bloom, filmic tone mapping); 0: faithful
    {"lr_Bloom", "1"}, {"lr_HDR", "0"},
    {"lr_ShadowSoftness", "3"},        // lamp radius in world units: 0 = hard shadows; larger = wider penumbra away from the caster
    {"lr_Volumetric", "0"},            // light scattered in the air between the eye and the surface (0 = off)        // bloom strength; 1 = HDR10 output when the display offers it
    {"lr_Bump", "1"}, {"lr_Specular", "1"},     // generated normal maps / highlights (not game data; 0 turns them off)
    {"lr_VSync", "1"}, {"lr_MaxFPS", "0"}, {"lr_DebugView", "0"}, {"lr_Exposure", "1"}, {"lr_Widescreen", "1"},
    {"lr_GameSpeedFix", "1"}, {"lr_MouseFix", "1"},
};
static void RegisterOptions() {
    char buf[96];
    for (auto &o : kOptions)
        if (!g_rs->GetConsoleVar((char *)o[0])) {
            sprintf(buf, "+%s %s", o[0], o[1]);     // leading '+': saved to autoexec.cfg
            g_rs->RunConsoleString(buf);
        }
}

// ------------------------------------------------------------------ exports
extern "C" __declspec(dllexport) LT1RMode *GetSupportedModes() {
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1600, 900}, {1920, 1000}};
    LT1RMode *head = nullptr;
    for (int i = (int)(sizeof(sizes) / sizeof(sizes[0])) - 1; i >= 0; i--) {
        LT1RMode *m = (LT1RMode *)calloc(1, sizeof(LT1RMode));
        m->m_bHardware = 1;
        strcpy(m->m_RenderDLL, "lithray.ren");
        strcpy(m->m_InternalName, "primary");
        strcpy(m->m_Description, "LithRay (Vulkan ray tracing)");
        m->m_Width = sizes[i][0]; m->m_Height = sizes[i][1]; m->m_BitDepth = 16;
        m->m_pNext = head; head = m;
    }
    return head;
}
extern "C" __declspec(dllexport) void FreeModeList(LT1RMode *m) {
    while (m) { LT1RMode *n = m->m_pNext; free(m); m = n; }
}

// ------------------------------------------------------------------ textures
static std::vector<LT1SharedTexture *> g_texSlots = {nullptr};   // index = id, 0 unused
static std::vector<uint32_t> g_texFree;

static void BindTexture(LT1SharedTexture *tex) {
    if (!tex || tex->m_pRenderData || !bridge::Alive()) return;
    LT1TextureData *data = (LT1TextureData *)g_rs->GetTexture(tex, nullptr);
    if (!data) return;
    __try {
        uint32_t w = data->m_BaseWidth, h = data->m_BaseHeight;
        bool alpha = (data->m_DtxFlags & lr::DTX_ALPHAMASKS) != 0;
        uint32_t mips = 0, bytes = 256 * 4;
        for (uint32_t i = 0; i < 4 && i < data->m_nMipmaps; i++) {
            const LT1TextureMip &m = data->m_Mips[i];
            uint32_t mw = (w >> i) ? (w >> i) : 1, mh = (h >> i) ? (h >> i) : 1;
            if (!m.m_Data || m.m_Width != mw || m.m_Height != mh || (alpha && !m.m_AlphaData)) break;
            bytes += mw * mh + (alpha ? (mw * mh + 1) / 2 : 0);
            mips++;
        }
        if (mips && data->m_pPalette && w && h) {
            uint32_t id;
            if (!g_texFree.empty()) { id = g_texFree.back(); g_texFree.pop_back(); }
            else { id = (uint32_t)g_texSlots.size(); g_texSlots.push_back(nullptr); }
            g_texSlots[id] = tex;
            auto *c = bridge::Emit<lr::TexUploadCmd>(lr::CmdTexUpload, bytes);
            c->id = id; c->width = w; c->height = h; c->nMips = mips; c->dtxFlags = data->m_DtxFlags;
            if (tex->m_pFile) {
                const char *name = *(const char **)((uint8_t *)tex->m_pFile + 0x1c);
                if (name && !IsBadStringPtrA(name, sizeof(c->name))) strncpy(c->name, name, sizeof(c->name) - 1);
            }
            uint8_t *p = (uint8_t *)(c + 1);
            memcpy(p, data->m_pPalette->m_Colors, 256 * 4); p += 256 * 4;
            for (uint32_t i = 0; i < mips; i++) {
                const LT1TextureMip &m = data->m_Mips[i];
                uint32_t n = m.m_Width * m.m_Height;
                if (m.m_Pitch == m.m_Width) memcpy(p, m.m_Data, n);
                else for (uint32_t y = 0; y < m.m_Height; y++) memcpy(p + y * m.m_Width, m.m_Data + y * m.m_Pitch, m.m_Width);
                p += n;
                if (alpha) { memcpy(p, m.m_AlphaData, (n + 1) / 2); p += (n + 1) / 2; }
            }
            tex->m_pRenderData = (void *)(uintptr_t)id;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { bridge::Log("BindTexture: bad texture data at %p", data); }
    g_rs->FreeTexture(tex);
}

uint32_t TexId(LT1SharedTexture *tex) {
    if (!tex) return 0;
    if (!tex->m_pRenderData) BindTexture(tex);     // lazy, like d3d.ren's reload when +0x08 is NULL
    return (uint32_t)(uintptr_t)tex->m_pRenderData;
}

static void UnbindTexture(LT1SharedTexture *tex) {
    if (!tex) return;
    uint32_t id = (uint32_t)(uintptr_t)tex->m_pRenderData;
    tex->m_pRenderData = nullptr;
    if (!id || id >= g_texSlots.size() || g_texSlots[id] != tex) return;
    g_texSlots[id] = nullptr;
    g_texFree.push_back(id);
    auto *c = bridge::Emit<lr::TexFreeCmd>(lr::CmdTexFree);
    c->id = id;
}

// ------------------------------------------------------------------ 2D layer
// LT1 draws menus, fonts, console and HUD into the same 16-bit back buffer as the 3D scene. Here that buffer is
// a CPU 565 image plus a coverage mask; the host composites it over the 3D frame.
struct RSurface { int width, height; uint16_t *pixels; };
static std::vector<uint16_t> g_screen, g_lockCopy;
static std::vector<uint8_t> g_mask;
static bool g_overlayDirty, g_screenLocked;
static LT1Rect g_lockRect;

static bool ClipToScreen(LT1Rect &r) {
    auto clamp = [](int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; };
    r.left = clamp(r.left, g_modeW); r.right = clamp(r.right, g_modeW);
    r.top = clamp(r.top, g_modeH); r.bottom = clamp(r.bottom, g_modeH);
    return r.left < r.right && r.top < r.bottom;
}
static void Clear(LT1Rect *rect, uint32_t flags) {
    if (!(flags & 1)) return;
    LT1Rect r = rect ? *rect : LT1Rect{0, 0, (int)g_modeW, (int)g_modeH};
    if (!ClipToScreen(r)) return;
    for (int y = r.top; y < r.bottom; y++) {
        memset(&g_screen[y * g_modeW + r.left], 0, (r.right - r.left) * 2);
        memset(&g_mask[y * g_modeW + r.left], 0, r.right - r.left);
    }
    g_overlayDirty = true;
}
static RSurface *CreateSurface(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    RSurface *s = (RSurface *)malloc(sizeof(RSurface));
    s->width = w; s->height = h;
    s->pixels = (uint16_t *)calloc((size_t)w * h, 2);
    return s;
}
static void DeleteSurface(RSurface *s) { if (s) { free(s->pixels); free(s); } }
static void GetSurfaceInfo(RSurface *s, uint32_t *w, uint32_t *h, long *pitch) {
    if (!s) return;
    if (w) *w = s->width;
    if (h) *h = s->height;
    if (pitch) *pitch = s->width * 2;
}
static void *LockSurface(RSurface *s) { return s ? s->pixels : nullptr; }
static void UnlockSurface(RSurface *) {}
static char OptimizeSurface(RSurface *, uint32_t) { return 1; }
static void UnoptimizeSurface(RSurface *) {}

static char LockScreen(int l, int t, int r, int b, void **data, long *pitch) {
    LT1Rect rc{l, t, r, b};
    if (g_screenLocked || !ClipToScreen(rc)) return 0;
    for (int y = rc.top; y < rc.bottom; y++)
        memcpy(&g_lockCopy[y * g_modeW + rc.left], &g_screen[y * g_modeW + rc.left], (rc.right - rc.left) * 2);
    if (data) *data = &g_screen[rc.top * g_modeW + rc.left];
    if (pitch) *pitch = g_modeW * 2;
    g_lockRect = rc; g_screenLocked = true;
    return 1;
}
static void UnlockScreen() {
    if (!g_screenLocked) return;
    for (int y = g_lockRect.top; y < g_lockRect.bottom; y++)
        for (int x = g_lockRect.left; x < g_lockRect.right; x++) {
            size_t i = y * g_modeW + x;
            if (g_screen[i] != g_lockCopy[i]) g_mask[i] = 255;
        }
    g_screenLocked = false; g_overlayDirty = true;
}
static void BlitToScreen(LT1BlitRequest *rq) {
    if (!rq || !rq->m_hBuffer || !rq->m_pSrcRect || !rq->m_pDestRect) return;
    RSurface *s = (RSurface *)rq->m_hBuffer;
    LT1Rect src = *rq->m_pSrcRect, dst = *rq->m_pDestRect;
    int sw = src.right - src.left, sh = src.bottom - src.top, dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    LT1Rect c = dst;
    if (!ClipToScreen(c)) return;
    bool keyed = (rq->m_BlitOptions & 1) != 0;
    uint16_t key = (uint16_t)rq->m_TransparentColor;
    for (int dy = c.top; dy < c.bottom; dy++) {
        int sy = src.top + (dy - dst.top) * sh / dh;
        if (sy < 0 || sy >= s->height) continue;
        const uint16_t *srow = s->pixels + sy * s->width;
        uint16_t *drow = &g_screen[dy * g_modeW];
        uint8_t *mrow = &g_mask[dy * g_modeW];
        for (int dx = c.left; dx < c.right; dx++) {
            int sx = src.left + (dx - dst.left) * sw / dw;
            if (sx < 0 || sx >= s->width) continue;
            uint16_t px = srow[sx];
            if (keyed && px == key) continue;
            drow[dx] = px; mrow[dx] = 255;
        }
    }
    g_overlayDirty = true;
}
static void SendOverlay() {
    if (!g_overlayDirty) return;
    g_overlayDirty = false;
    size_t n = (size_t)g_modeW * g_modeH;
    bool empty = true;
    for (size_t i = 0; i < n; i += 4) if (*(uint32_t *)&g_mask[i]) { empty = false; break; }
    auto *c = bridge::Emit<lr::OverlayCmd>(lr::CmdOverlay, empty ? 0 : (uint32_t)(n * 3));
    c->width = g_modeW; c->height = g_modeH; c->empty = empty;
    if (!empty) {
        memcpy(c + 1, g_screen.data(), n * 2);
        memcpy((uint8_t *)(c + 1) + n * 2, g_mask.data(), n);
    }
}

// ------------------------------------------------------------------ test automation
// lithray_cmd.txt in the game directory is polled; each line is a console command, or:
//   key <name|vk>   wait <ms>   waitworld   shot <file.png>   quit
static std::vector<std::string> g_script;
static DWORD g_scriptWaitUntil, g_scriptPoll;
static void PostKey(const char *name) {
    static const struct { const char *n; int vk; } keys[] = {{"enter", VK_RETURN}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
        {"space", VK_SPACE}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"tab", VK_TAB},
        {"f8", VK_F8}, {"tilde", VK_OEM_3}};
    int vk = 0;
    for (auto &k : keys) if (!_stricmp(k.n, name)) vk = k.vk;
    if (!vk) vk = name[1] ? atoi(name) : toupper((unsigned char)name[0]);
    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    PostMessageA(g_window, WM_KEYDOWN, vk, 1 | (scan << 16));
    PostMessageA(g_window, WM_KEYUP, vk, 1 | (scan << 16) | (1u << 30) | (1u << 31));
}
static void RunScript() {
    DWORD now = timeGetTime();
    // LITHRAY_CMD names another command file (a second, development instance next to a played one)
    static char cmdFile[MAX_PATH] = "";
    if (!cmdFile[0] && !GetEnvironmentVariableA("LITHRAY_CMD", cmdFile, sizeof(cmdFile))) strcpy(cmdFile, "lithray_cmd.txt");
    if (g_script.empty() && now - g_scriptPoll > 250) {
        g_scriptPoll = now;
        if (FILE *f = fopen(cmdFile, "r")) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
                if (n) g_script.push_back(line);
            }
            fclose(f);
            DeleteFileA(cmdFile);
        }
    }
    while (!g_script.empty() && (int)(now - g_scriptWaitUntil) >= 0) {
        std::string line = g_script.front();
        g_script.erase(g_script.begin());
        if (!strncmp(line.c_str(), "wait ", 5)) { g_scriptWaitUntil = now + atoi(line.c_str() + 5); break; }
        else if (line == "waitworld") {         // until a world is loaded and 3D scenes are being rendered
            if (!Scene_HaveWorld() || !g_hadScene) { g_script.insert(g_script.begin(), line); break; }
        }
        else if (!strncmp(line.c_str(), "key ", 4)) PostKey(line.c_str() + 4);
        else if (!strncmp(line.c_str(), "shot ", 5)) {
            auto *c = bridge::Emit<lr::ScreenshotCmd>(lr::CmdScreenshot);
            GetFullPathNameA(line.c_str() + 5, sizeof(c->path), c->path, nullptr);
            c->format = 1;
        } else if (line == "quit") PostMessageA(g_window, WM_CLOSE, 0, 0);
        else g_rs->RunConsoleString((char *)line.c_str());
    }
}

// ------------------------------------------------------------------ slots
static void SetupWindow(bool windowed) {
    if (windowed) {
        RECT r = {0, 0, (LONG)g_modeW, (LONG)g_modeH};
        AdjustWindowRectEx(&r, GetWindowLongA(g_window, GWL_STYLE), FALSE, GetWindowLongA(g_window, GWL_EXSTYLE));
        SetWindowPos(g_window, HWND_NOTOPMOST, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOACTIVATE);
    } else {
        // Borderless on the primary monitor at 0,0: the engine recentres the cursor at width/2, height/2 in
        // screen coordinates, which assumes the window starts at the origin.
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoA(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongA(g_window, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
        SetWindowLongA(g_window, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(g_window, g_background ? HWND_BOTTOM : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOCOPYBITS | SWP_SHOWWINDOW | (g_background ? SWP_NOACTIVATE : 0));
    }
}
static void BringToForeground() {
    HWND fg = GetForegroundWindow();
    if (fg == g_window) return;
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = other && other != me && AttachThreadInput(me, other, TRUE);
    BringWindowToTop(g_window);
    SetForegroundWindow(g_window);
    if (attached) AttachThreadInput(me, other, FALSE);
}

static int Init(LT1RenderStructInit *init) {
    bridge::Log("Init %ux%u hwnd %p", init->m_Mode.m_Width, init->m_Mode.m_Height, init->m_hWnd);
    g_window = (HWND)init->m_hWnd;
    g_modeW = init->m_Mode.m_Width; g_modeH = init->m_Mode.m_Height;
    g_rs->m_Width = g_modeW; g_rs->m_Height = g_modeH;
    RegisterOptions();
    g_background = CVar("lr_Background", 0) != 0;
    bool windowed = CVar("windowed", 0) != 0;

    if (!bridge::Connect()) {
        MessageBoxA(g_window, "LithRay: the 64-bit render host could not be started.\nSee lithray_shim.log and lithray_host.log.",
                    "LithRay", MB_OK | MB_ICONERROR);
        return 1;
    }
    SetupWindow(windowed);

    g_screen.assign((size_t)g_modeW * g_modeH, 0);
    g_lockCopy.assign((size_t)g_modeW * g_modeH, 0);
    g_mask.assign(((size_t)g_modeW * g_modeH + 3) & ~3u, 0);
    g_overlayDirty = true; g_screenLocked = false; g_in3D = false; g_hadScene = false;

    auto *c = bridge::Emit<lr::InitCmd>(lr::CmdInit);
    c->hwnd = (uint64_t)(uintptr_t)g_window; c->modeWidth = g_modeW; c->modeHeight = g_modeH; c->windowed = windowed;
    bridge::Flush();

    if (!g_cursorSlot) g_cursorSlot = PatchImport("user32.dll", "SetCursorPos", (void *)&GuardedSetCursorPos, &g_cursorOrig);
    if (CVar("lr_MouseFix", 1) != 0 && !g_tickSlot) {
        g_tickOffset = GetTickCount() - timeGetTime() - 16;
        g_tickSlot = PatchImport("kernel32.dll", "GetTickCount", (void *)&FineTickCount, &g_tickOrig);
    }
    timeBeginPeriod(1);
    g_speedFixed = false; g_speedWanted = CVar("lr_GameSpeedFix", 1) != 0;
    TryGameSpeedFix(true);
    if (g_background) {
        if (!g_engineProc) {
            g_filteredWindow = g_window;
            g_engineProc = (WNDPROC)SetWindowLongA(g_window, GWL_WNDPROC, (LONG)&FocusFilterProc);
        }
        PostMessageA(g_window, WM_ACTIVATEAPP, TRUE, 0);   // the engine only runs its client once told it is active
    } else
        BringToForeground();
    UpdateCursorCapture();      // hidden and centred with focus, visible and free without

    lr::SharedHeader *h = bridge::Header();
    strncpy(init->m_Mode.m_InternalName, "primary", sizeof(init->m_Mode.m_InternalName) - 1);
    _snprintf(init->m_Mode.m_Description, sizeof(init->m_Mode.m_Description) - 1, "LithRay: %s", h->deviceName);
    init->m_Mode.m_bHardware = 1;
    return 0;
}

static void Term() {
    bridge::Log("Term");
    Scene_Reset();
    bridge::Emit<lr::CmdHeader>(lr::CmdTerm);
    bridge::Flush();
    for (auto *&t : g_texSlots) if (t) { t->m_pRenderData = nullptr; t = nullptr; }
    g_texSlots.assign(1, nullptr); g_texFree.clear();
    RemoveFocusFilter();
    RestoreImport(g_cursorSlot, g_cursorOrig);
    RestoreImport(g_tickSlot, g_tickOrig);
    if (g_cursorCaptured) { ShowCursor(TRUE); g_cursorCaptured = false; }
    timeEndPeriod(1);
    bridge::Disconnect();
}

static void SetSoftSky(LT1SharedTexture **) {}
static char QueryDeletePalette(LT1DEPalette *) { return 1; }
static char SetMasterPalette(LT1SharedTexture *) { return 1; }
static int Start3D() { g_in3D = true; return 1; }
static int End3D() { g_in3D = false; return 1; }
static char IsIn3D() { return g_in3D; }
static int StartOptimized2D() { return 1; }
static void EndOptimized2D() {}
static char IsInOptimized2D() { return 0; }
static void RenderCommand(int, char **) {}
static void *GetHook(char *) { return nullptr; }       // no DirectDraw: Smacker videos are skipped
static int GetInfoFlags() { return 0; }
static int GetBufferFormat() { return 0; }              // RGB565
static void ReadConsoleVariables() {}

static int RenderScene(LT1SceneDesc *scene) {
    if (!scene || !bridge::Alive()) return 0;
    __try {
        Scene_Render(scene);
        g_hadScene = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bridge::Log("RenderScene: exception %08lx while reading engine data", GetExceptionCode());
    }
    return 0;
}

static DWORD g_lastSwap;
static void SwapBuffersSlot() {
    UpdateCursorCapture();
    static DWORD lastTry;
    if (g_speedWanted && timeGetTime() - lastTry > 1000) { lastTry = timeGetTime(); TryGameSpeedFix(false); }
    RunScript();
    SendOverlay();
    auto *p = bridge::Emit<lr::PresentCmd>(lr::CmdPresent);
    p->hadScene = g_hadScene; g_hadScene = false;
    bridge::Flush();

    float cap = CVar("lr_MaxFPS", 0);
    if (!g_speedFixed && (cap <= 0 || cap > 100)) cap = 100;
    if (cap > 0) {
        DWORD frame = (DWORD)(1000.0f / cap + 0.5f), now = timeGetTime();
        if (now - g_lastSwap < frame) Sleep(frame - (now - g_lastSwap));
    }
    g_lastSwap = timeGetTime();
}

static void MakeScreenShot(const char *name) {
    if (!name) return;
    auto *c = bridge::Emit<lr::ScreenshotCmd>(lr::CmdScreenshot);
    GetFullPathNameA(name, sizeof(c->path), c->path, nullptr);
    c->format = 0;
}

static void *CreateContext(LT1RenderContextInit *init) { return Scene_CreateContext(init); }
static void DeleteContext(void *ctx) { Scene_DeleteContext(ctx); }

extern "C" __declspec(dllexport) void RenderDLLSetup(LT1RenderStruct *rs) {
    g_rs = rs;
    void **s = rs->m_Slots;
    s[LT1RS_Init] = (void *)&Init; s[LT1RS_Term] = (void *)&Term; s[LT1RS_SetSoftSky] = (void *)&SetSoftSky;
    s[LT1RS_BindTexture] = (void *)&BindTexture; s[LT1RS_UnbindTexture] = (void *)&UnbindTexture;
    s[LT1RS_QueryDeletePalette] = (void *)&QueryDeletePalette; s[LT1RS_SetMasterPalette] = (void *)&SetMasterPalette;
    s[LT1RS_CreateContext] = (void *)&CreateContext; s[LT1RS_DeleteContext] = (void *)&DeleteContext;
    s[LT1RS_Clear] = (void *)&Clear; s[LT1RS_Start3D] = (void *)&Start3D; s[LT1RS_End3D] = (void *)&End3D;
    s[LT1RS_IsIn3D] = (void *)&IsIn3D; s[LT1RS_StartOptimized2D] = (void *)&StartOptimized2D;
    s[LT1RS_EndOptimized2D] = (void *)&EndOptimized2D; s[LT1RS_IsInOptimized2D] = (void *)&IsInOptimized2D;
    s[LT1RS_RenderScene] = (void *)&RenderScene; s[LT1RS_RenderCommand] = (void *)&RenderCommand;
    s[LT1RS_GetHook] = (void *)&GetHook; s[LT1RS_SwapBuffers] = (void *)&SwapBuffersSlot;
    s[LT1RS_GetInfoFlags] = (void *)&GetInfoFlags; s[LT1RS_GetBufferFormat] = (void *)&GetBufferFormat;
    s[LT1RS_CreateSurface] = (void *)&CreateSurface; s[LT1RS_DeleteSurface] = (void *)&DeleteSurface;
    s[LT1RS_GetSurfaceInfo] = (void *)&GetSurfaceInfo; s[LT1RS_LockSurface] = (void *)&LockSurface;
    s[LT1RS_UnlockSurface] = (void *)&UnlockSurface; s[LT1RS_OptimizeSurface] = (void *)&OptimizeSurface;
    s[LT1RS_UnoptimizeSurface] = (void *)&UnoptimizeSurface; s[LT1RS_LockScreen] = (void *)&LockScreen;
    s[LT1RS_UnlockScreen] = (void *)&UnlockScreen; s[LT1RS_BlitToScreen] = (void *)&BlitToScreen;
    s[LT1RS_MakeScreenShot] = (void *)&MakeScreenShot; s[LT1RS_ReadConsoleVariables] = (void *)&ReadConsoleVariables;
}
