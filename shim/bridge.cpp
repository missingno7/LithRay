#include "bridge.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

namespace bridge {

static HANDLE g_map, g_evSubmit, g_evDone, g_hostProcess;
static uint8_t *g_view;
static lr::SharedHeader *g_hdr;
static uint8_t *g_arena;
static uint32_t g_used;
static bool g_alive;
static uint8_t *g_sink;         // writes land here when the host is gone
static uint32_t g_sinkSize;
static FILE *g_log;

void Log(const char *fmt, ...) {
    if (!g_log) {
        g_log = fopen("lithray_shim.log", "a");
        if (!g_log) return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static void Name(wchar_t *out, const wchar_t *what) {
    swprintf(out, 128, LITHRAY_OBJECT_FMT, (unsigned)GetCurrentProcessId(), what);
}

static void CloseAll() {
    if (g_view) UnmapViewOfFile(g_view);
    if (g_map) CloseHandle(g_map);
    if (g_evSubmit) CloseHandle(g_evSubmit);
    if (g_evDone) CloseHandle(g_evDone);
    if (g_hostProcess) CloseHandle(g_hostProcess);
    g_view = nullptr; g_map = g_evSubmit = g_evDone = g_hostProcess = nullptr;
    g_hdr = nullptr; g_arena = nullptr; g_alive = false; g_used = 0;
}

static bool SpawnHost() {
    // lithray_host.exe lives next to this DLL (the game directory) unless LITHRAY_HOST names another one.
    wchar_t exe[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LITHRAY_HOST", exe, MAX_PATH)) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&SpawnHost, &self);
        GetModuleFileNameW(self, exe, MAX_PATH);
        wchar_t *slash = wcsrchr(exe, L'\\');
        wcscpy(slash ? slash + 1 : exe, L"lithray_host.exe");
    }
    wchar_t cmd[MAX_PATH + 64];
    swprintf(cmd, MAX_PATH + 64, L"\"%s\" --game-pid %u", exe, (unsigned)GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log("CreateProcess(%ls) failed: %lu", exe, GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    g_hostProcess = pi.hProcess;
    Log("host started: pid %lu (%ls)", pi.dwProcessId, exe);
    return true;
}

bool Connect() {
    if (!g_sink) { g_sinkSize = 1u << 20; g_sink = (uint8_t *)malloc(g_sinkSize); }
    CloseAll();
    wchar_t name[128];
    Name(name, L"map");
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, lr::kHeaderSize + lr::kArenaSize, name);
    if (!g_map) { Log("CreateFileMapping failed: %lu", GetLastError()); return false; }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_view = (uint8_t *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_view) { Log("MapViewOfFile failed: %lu", GetLastError()); CloseAll(); return false; }
    g_hdr = (lr::SharedHeader *)g_view;
    g_arena = g_view + lr::kHeaderSize;
    Name(name, L"submit"); g_evSubmit = CreateEventW(nullptr, FALSE, FALSE, name);
    Name(name, L"done");   g_evDone = CreateEventW(nullptr, FALSE, FALSE, name);

    // A host from an earlier renderer instance of this game process (Alt+Tab, mode switch) is reused.
    if (existed && g_hdr->magic == lr::kMagic && g_hdr->version == lr::kVersion && g_hdr->hostPid) {
        g_hostProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_hdr->hostPid);
        if (g_hostProcess && WaitForSingleObject(g_hostProcess, 0) == WAIT_TIMEOUT && g_hdr->hostState == lr::HostReady) {
            Log("reconnected to host pid %u", g_hdr->hostPid);
            ResetEvent(g_evDone);
            g_alive = true; g_used = 0;
            return true;
        }
        if (g_hostProcess) { CloseHandle(g_hostProcess); g_hostProcess = nullptr; }
    }
    memset(g_hdr, 0, sizeof(*g_hdr));
    g_hdr->magic = lr::kMagic; g_hdr->version = lr::kVersion; g_hdr->arenaSize = lr::kArenaSize;
    g_hdr->gamePid = GetCurrentProcessId();
    if (!SpawnHost()) { CloseAll(); return false; }
    // Wait for the device to come up (shader/pipeline creation included).
    for (int i = 0; i < 3000; i++) {
        if (g_hdr->hostState == lr::HostReady) break;
        if (g_hdr->hostState == lr::HostFailed || WaitForSingleObject(g_hostProcess, 0) != WAIT_TIMEOUT) {
            Log("host failed to start: %s", g_hdr->hostMessage);
            CloseAll(); return false;
        }
        Sleep(10);
    }
    if (g_hdr->hostState != lr::HostReady) { Log("host start timed out"); CloseAll(); return false; }
    Log("host ready: %s, ray tracing %s", g_hdr->deviceName, g_hdr->rtSupported ? "yes" : "no");
    g_alive = true; g_used = 0;
    return true;
}

void Disconnect() { CloseAll(); }
bool Alive() { return g_alive; }
lr::SharedHeader *Header() { return g_hdr; }

void Flush(bool wait) {
    if (!g_alive) { g_used = 0; return; }
    if (!g_used) return;
    g_hdr->used = g_used;
    g_hdr->flushCount++;
    SetEvent(g_evSubmit);
    if (wait) {
        HANDLE h[2] = {g_evDone, g_hostProcess};
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, h, FALSE, 30000);
            if (r == WAIT_OBJECT_0) break;
            Log("host %s: %s", r == WAIT_TIMEOUT ? "stopped answering" : "exited", g_hdr->hostMessage);
            g_alive = false;
            break;
        }
    }
    g_used = 0;
}

uint32_t Room() { return lr::kArenaSize - (uint32_t)sizeof(lr::CmdHeader) - 64; }

void *Begin(lr::Cmd type, uint32_t payload) {
    uint32_t size = lr::Align8((uint32_t)sizeof(lr::CmdHeader) + payload);
    if (!g_alive || size > lr::kArenaSize) {
        if (g_alive) Log("command %u too large (%u bytes): dropped", (unsigned)type, size);
        if (size > g_sinkSize) { g_sinkSize = size; g_sink = (uint8_t *)realloc(g_sink, g_sinkSize); }
        return g_sink + sizeof(lr::CmdHeader);
    }
    if (g_used + size > lr::kArenaSize) Flush();
    if (!g_alive) return Begin(type, payload);
    lr::CmdHeader *h = (lr::CmdHeader *)(g_arena + g_used);
    h->type = type; h->size = size;
    g_used += size;
    return h + 1;
}

void End(void *payloadStart, uint32_t actualPayload) {
    if (!g_alive) return;
    lr::CmdHeader *h = (lr::CmdHeader *)payloadStart - 1;
    if ((uint8_t *)h < g_arena || (uint8_t *)h >= g_arena + lr::kArenaSize) return;
    uint32_t size = lr::Align8((uint32_t)sizeof(lr::CmdHeader) + actualPayload);
    if (size > h->size) { Log("End: command grew (%u > %u)", size, h->size); return; }
    g_used -= h->size - size;
    h->size = size;
}

} // namespace bridge
