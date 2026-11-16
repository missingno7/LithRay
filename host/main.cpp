// lithray_host.exe: the 64-bit render process. Started by the shim with --game-pid; attaches to the shared
// mapping, brings the Vulkan device up, then loops: wait for a flush, decode (copy), acknowledge, render.
#include "renderer.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static FILE *g_log;
static lr::SharedHeader *g_hdr;

void Log(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}
void Fatal(const char *fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    Log("FATAL: %s", buf);
    if (g_hdr) { strncpy(g_hdr->hostMessage, buf, sizeof(g_hdr->hostMessage) - 1); g_hdr->hostState = lr::HostFailed; }
    ExitProcess(2);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *e) {
    Log("CRASH: exception %08lx at %p", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    if (g_hdr) { strcpy(g_hdr->hostMessage, "render host crashed (see lithray_host.log)"); g_hdr->hostState = lr::HostFailed; }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    DWORD gamePid = 0;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--game-pid")) gamePid = strtoul(argv[i + 1], nullptr, 10);
    char logName[MAX_PATH] = "lithray_host.log";
    GetEnvironmentVariableA("LITHRAY_HOST_LOG", logName, sizeof(logName));
    g_log = fopen(logName, "w");
    SetUnhandledExceptionFilter(CrashFilter);
    if (!gamePid) { Log("usage: lithray_host --game-pid <pid> (started by lithray.ren)"); return 1; }

    wchar_t name[128];
    swprintf(name, 128, LITHRAY_OBJECT_FMT, (unsigned)gamePid, L"map");
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) { Log("cannot open the shared mapping: %lu", GetLastError()); return 1; }
    uint8_t *view = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) { Log("cannot map the shared memory: %lu", GetLastError()); return 1; }
    g_hdr = (lr::SharedHeader *)view;
    uint8_t *arena = view + lr::kHeaderSize;
    if (g_hdr->magic != lr::kMagic || g_hdr->version != lr::kVersion) Fatal("protocol mismatch: shim %u, host %u", g_hdr->version, lr::kVersion);
    swprintf(name, 128, LITHRAY_OBJECT_FMT, (unsigned)gamePid, L"submit");
    HANDLE evSubmit = CreateEventW(nullptr, FALSE, FALSE, name);
    swprintf(name, 128, LITHRAY_OBJECT_FMT, (unsigned)gamePid, L"done");
    HANDLE evDone = CreateEventW(nullptr, FALSE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    if (!game) Fatal("cannot open the game process: %lu", GetLastError());

    static Renderer r;      // large: keep it off the stack
    r.init();
    g_hdr->hostPid = GetCurrentProcessId();
    g_hdr->rtSupported = r.gfx.rt;
    strncpy(g_hdr->deviceName, r.gfx.deviceName, sizeof(g_hdr->deviceName) - 1);
    g_hdr->hostState = lr::HostReady;
    Log("ready (game pid %lu)", gamePid);

    HANDLE waits[2] = {evSubmit, game};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) { Log("game process ended"); break; }
        bool present = false, hadScene = false;
        uint32_t used = g_hdr->used;
        for (uint32_t off = 0; off + sizeof(lr::CmdHeader) <= used;) {
            const lr::CmdHeader *h = (const lr::CmdHeader *)(arena + off);
            if (h->size < sizeof(lr::CmdHeader) || off + h->size > used) { Log("corrupt command stream at %u", off); break; }
            const uint8_t *body = (const uint8_t *)(h + 1);
            switch (h->type) {
            case lr::CmdInit: r.onInit(*(const lr::InitCmd *)body); break;
            case lr::CmdTerm: r.onTerm(); break;
            case lr::CmdTexUpload: r.onTexUpload(*(const lr::TexUploadCmd *)body, body + sizeof(lr::TexUploadCmd)); break;
            case lr::CmdTexFree: r.onTexFree(((const lr::TexFreeCmd *)body)->id); break;
            case lr::CmdWorldBegin: r.onWorldBegin(*(const lr::WorldBeginCmd *)body, body + sizeof(lr::WorldBeginCmd)); break;
            case lr::CmdWorldEnd: r.onWorldEnd(); break;
            case lr::CmdModelDef: r.onModelDef(*(const lr::ModelDefCmd *)body, body + sizeof(lr::ModelDefCmd)); break;
            case lr::CmdModelFree: r.onModelFree(((const lr::ModelFreeCmd *)body)->id); break;
            case lr::CmdScene: r.onScene(*(const lr::SceneCmd *)body, h->size - (uint32_t)sizeof(lr::CmdHeader)); break;
            case lr::CmdOverlay: r.onOverlay(*(const lr::OverlayCmd *)body, body + sizeof(lr::OverlayCmd)); break;
            case lr::CmdScreenshot: r.onScreenshot(*(const lr::ScreenshotCmd *)body); break;
            case lr::CmdPresent: present = true; hadScene = ((const lr::PresentCmd *)body)->hadScene != 0; break;
            default: Log("unknown command %u", h->type); break;
            }
            off += h->size;
        }
        g_hdr->swapWidth = r.gfx.extent.width; g_hdr->swapHeight = r.gfx.extent.height;
        g_hdr->hostFrameMs = r.lastFrameMs; g_hdr->hostGpuMs = r.lastGpuMs;
        g_hdr->ackCount = g_hdr->flushCount;
        SetEvent(evDone);                   // the game continues with its next frame while this one is drawn
        if (present) r.renderFrame(hadScene);
    }
    g_hdr->hostState = lr::HostExited;
    r.shutdown();
    return 0;
}
