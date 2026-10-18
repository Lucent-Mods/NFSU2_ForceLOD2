// NFSU2 ForceLOD 2 - entry point and log.
//
// Loaded by an ASI loader (scripts\NFSU2_ForceLOD2.asi). No dinput8 proxy: never replace dinput8.dll.
#include "core.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>

char g_baseDir[MAX_PATH];

// Small stack on purpose: Log can run on game threads (memory / file hooks). Formatting into a static buffer under a
// lock, then one WriteFile - no CRT stream (its text conversion alone puts 5 KB on the stack).
void LogRaw(const char* text, int len)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\NFSU2_ForceLOD2.log", g_baseDir);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, text, len, &w, nullptr);
    CloseHandle(f);
}

void Log(const char* fmt, ...)
{
    if (!g_cfg.log) return;
    static CRITICAL_SECTION cs; static volatile LONG init = 0;
    if (InterlockedCompareExchange(&init, 1, 0) == 0) { InitializeCriticalSection(&cs); init = 2; }
    while (init != 2) Sleep(0);
    EnterCriticalSection(&cs);
    static char line[1100];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = snprintf(line, 16, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m; if (n > static_cast<int>(sizeof(line)) - 2) n = sizeof(line) - 2;
    line[n++] = '\r'; line[n++] = '\n';
    LogRaw(line, n);
    LeaveCriticalSection(&cs);
}

namespace {
// SPEED2.EXE v1.2 (4 800 512 B, base 0x400000, no relocations): a few byte runs every module depends on.
bool VerifyGame()
{
    if (reinterpret_cast<DWORD>(GetModuleHandleA(nullptr)) != 0x400000) return false;
    static const BYTE kLodSite[6] = { 0x8B, 0x5B, 0x44, 0xF6, 0xC7, 0x18 };   // 0x5D6653 (or our JMP from an earlier instance)
    static const BYTE kResume[4]  = { 0x85, 0xDB, 0x0F, 0x84 };               // 0x5D673A
    static const BYTE kSettings[1] = { 0xA0 };                                 // 0x5D2850 mov al,[...]
    const BYTE* lod = reinterpret_cast<const BYTE*>(0x5D6653);
    return memcmp(reinterpret_cast<const void*>(0x5D673A), kResume, 4) == 0 &&
           (memcmp(lod, kLodSite, 6) == 0 || lod[0] == 0xE9) &&
           (*reinterpret_cast<const BYTE*>(0x5D2850) == kSettings[0] || *reinterpret_cast<const BYTE*>(0x5D2850) == 0xE9);
}
} // namespace

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    GetModuleFileNameA(self, g_baseDir, MAX_PATH);
    if (char* slash = strrchr(g_baseDir, '\\')) *slash = 0;
    ConfigLoad();
    Log("==== NFSU2_ForceLOD2 " FL2_VERSION " (build " __DATE__ " " __TIME__ "), profile %s ====", ProfileName(g_cfg.profile));
    if (!VerifyGame()) { Log("SPEED2.EXE is not the expected v1.2 image - nothing patched"); return TRUE; }
    DiagInstall(self);
    MemFixInstall();   // first: the low heap must be reserved while the space below 2 GB is still free
    PoolsInstall();
    FullMapInstall();
    LodInstall();
    SettingsInstall();
    ReflectionsInstall();
    DespawnInstall();
    UiInstall(self);
    PerfInstall();
    Log("startup done: %d patch sites registered", PatchCount());
    return TRUE;
}
