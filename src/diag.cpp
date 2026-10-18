// Diagnostics: crash log, hang log, RTX Remix detection.
//
// Crash log: an unhandled-exception filter, chained to the previous one. Unlike a vectored handler it
// only sees exceptions nobody handled - other plugins' first-chance exceptions (and our own __try in
// lod.cpp) never show up. Never re-asserted: SPEED2's own CRT filter forwards to the one it replaced, so adopting it
// as our "previous" would make the two call each other forever.
// Hang log: a thread that only READS globals (no thread suspension, no dumps - suspending threads during
// the Remix bridge's IPC can stall it). It logs when the streamer waits for sections for a long time.
#include "core.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

void LogRaw(const char* text, int len);   // core.cpp

namespace {
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
bool g_remix = false;
char g_remixText[128] = "no RTX Remix";
LONG g_crashes = 0;

const char* ModuleAt(DWORD addr, char* buf, size_t len)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &m) && m) {
        char path[MAX_PATH]; GetModuleFileNameA(m, path, MAX_PATH);
        const char* s = strrchr(path, '\\');
        snprintf(buf, len, "%s+0x%X", s ? s + 1 : path, addr - reinterpret_cast<DWORD>(m));
    } else snprintf(buf, len, "<no module>");
    return buf;
}

bool Readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<const BYTE*>(p) + n <= reinterpret_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (InterlockedIncrement(&g_crashes) == 1) {
        CONTEXT* c = ep->ContextRecord; char mod[160];
        Log("=== UNHANDLED EXCEPTION 0x%08X at 0x%08X (%s) thread %u ===", ep->ExceptionRecord->ExceptionCode, c->Eip, ModuleAt(c->Eip, mod, sizeof(mod)), GetCurrentThreadId());
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
            Log("  %s 0x%08X", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]));
        Log("  eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
        for (int i = 0, shown = 0; i < 1024 && shown < 24 && Readable(sp + i, 4); ++i) {
            DWORD v = sp[i];
            if ((v >= 0x00401000 && v < 0x00783000) || (v > 0x10000000 && Readable(reinterpret_cast<void*>(v), 1) && ModuleAt(v, mod, sizeof(mod))[0] != '<')) {
                Log("  stack[%3d] 0x%08X %s", i, v, ModuleAt(v, mod, sizeof(mod))); ++shown;
            }
        }
        char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\NFSU2_ForceLOD2_crash.dmp", g_baseDir);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
            BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs), &mei, nullptr, nullptr);
            CloseHandle(f);
            Log("  minidump %s: %s", path, ok ? "written" : "FAILED");
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Stack overflow: no unhandled-exception filter can run then (no stack left). A vectored handler sees it first, on the
// small reserve the OS leaves after the guard page; it writes the call chain (return addresses inside SPEED2.EXE and
// the .asi/.dll modules) with static buffers only, so the recursion can be named.
LONG g_soLogged = 0;
char g_soBuf[4096];

int AppendHex(char* p, DWORD v) { static const char h[] = "0123456789ABCDEF"; for (int i = 7; i >= 0; --i) p[7 - i] = h[(v >> (i * 4)) & 15]; return 8; }
int AppendStr(char* p, const char* s) { int n = 0; while (s[n]) { p[n] = s[n]; ++n; } return n; }

LONG WINAPI StackOverflowHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_soLogged) != 1) return EXCEPTION_CONTINUE_SEARCH;
    char* p = g_soBuf; int n = 0;
    n += AppendStr(p + n, "=== STACK OVERFLOW at ");
    n += AppendHex(p + n, ep->ContextRecord->Eip);
    n += AppendStr(p + n, " thread ");
    n += AppendHex(p + n, GetCurrentThreadId());
    n += AppendStr(p + n, ", return addresses from the top (game 004..007, others = module code):\r\n");
    const DWORD* sp = reinterpret_cast<const DWORD*>(ep->ContextRecord->Esp);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&StackOverflowHandler), &self);
    const DWORD selfLo = reinterpret_cast<DWORD>(self), selfHi = selfLo + 0x200000;
    int shown = 0;
    for (int i = 0; i < 16384 && shown < 120 && n < static_cast<int>(sizeof(g_soBuf)) - 32; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if ((i & 1023) == 0 && (!VirtualQuery(sp + i, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)) break;
        DWORD v = sp[i];
        bool game = v >= 0x00401000 && v < 0x00783000;
        bool ours = v >= selfLo + 0x1000 && v < selfHi;
        if (!game && !ours) continue;
        n += AppendStr(p + n, ours ? " asi+" : " ");
        n += AppendHex(p + n, ours ? v - selfLo : v);
        if (++shown % 8 == 0) n += AppendStr(p + n, "\r\n");
    }
    n += AppendStr(p + n, "\r\n");
    LogRaw(p, n);
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI HangThread(LPVOID)
{
    const BYTE* st = reinterpret_cast<const BYTE*>(0x0088EB00);   // TrackStreamer
    DWORD waitSince = 0, lastLog = 0; int lastState = -1;
    for (;;) {
        Sleep(1000);
        int state = *reinterpret_cast<const int*>(0x008654A4);
        int wanted = *reinterpret_cast<const int*>(st + 0x298), loaded = *reinterpret_cast<const int*>(st + 0x24), deferred = *reinterpret_cast<const int*>(st + 0x28);
        DWORD now = GetTickCount();
        if (state != lastState) { Log("[state] game state %d -> %d (streamer wanted %d loaded %d deferred %d)", lastState, state, wanted, loaded, deferred); lastState = state; }
        bool waiting = state != 6 && (wanted != loaded || deferred > 0);   // only outside free driving / racing (6)
        if (!waiting) { waitSince = 0; continue; }
        if (!waitSince) waitSince = now;
        if (now - waitSince >= static_cast<DWORD>(g_cfg.hangWatchdog) * 1000 && now - lastLog >= 30000) {
            lastLog = now;
            Log("[HANG?] streamer waits %u s: wanted %d loaded %d deferred %d, game state %d, pool base 0x%08X size 0x%X",
                (now - waitSince) / 1000, wanted, loaded, deferred, state, *reinterpret_cast<const DWORD*>(st + 0x69C), *reinterpret_cast<const DWORD*>(st + 0x6A0));
        }
    }
}

void DetectRemix()
{
    char exeDir[MAX_PATH]; GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = 0;
    char trex[MAX_PATH]; snprintf(trex, sizeof(trex), "%s\\.trex", exeDir);
    DWORD a = GetFileAttributesA(trex);
    bool trexDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    char d3d9[MAX_PATH]; snprintf(d3d9, sizeof(d3d9), "%s\\d3d9.dll", exeDir);
    bool localD3d9 = GetFileAttributesA(d3d9) != INVALID_FILE_ATTRIBUTES;
    g_remix = trexDir && localD3d9;
    snprintf(g_remixText, sizeof(g_remixText), g_remix ? "RTX Remix detected (.trex + d3d9.dll in the game folder)"
                                                       : (localD3d9 ? "local d3d9.dll without .trex (not Remix)" : "no RTX Remix"));
    Log("[INFO] %s", g_remixText);
    if (g_remix && g_cfg.profile != PROFILE_REMIX) Log("[WARN] RTX Remix found but profile is Vanilla - reflections / decals stay on");
}

// Remix setup check (docs REMIX_REQUIREMENTS §5 SOLLTE 6): every NFSU2 Remix project runs the game in fixed function
// (dxvk.conf d3d9.shaderModel = 0) - with shaders Remix drops the game's draws. Read once at startup, shown in the menu.
char g_warn[4][160]; int g_warnCount = 0;
void Warn(const char* text) { if (g_warnCount < 4) { snprintf(g_warn[g_warnCount++], sizeof(g_warn[0]), "%s", text); Log("[WARN] %s", text); } }

// value of `key` in a dxvk/rtx style conf ("key = value", '#' comments); false = file or key missing
bool ConfValue(const char* file, const char* key, char* out, size_t len)
{
    FILE* f = fopen(file, "r");
    if (!f) return false;
    char line[512]; bool found = false;
    const size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || _strnicmp(p, key, kl) != 0) continue;
        p += kl; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p; while (*p == ' ' || *p == '\t') ++p;
        size_t n = strcspn(p, " \t\r\n#");
        if (n >= len) n = len - 1;
        memcpy(out, p, n); out[n] = 0; found = true;   // the last occurrence wins, like dxvk
    }
    fclose(f);
    return found;
}

void CheckRemixSetup()
{
    if (!g_remix) return;
    char exeDir[MAX_PATH]; GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char* s = strrchr(exeDir, '\\')) *s = 0;
    char dxvk[MAX_PATH]; snprintf(dxvk, sizeof(dxvk), "%s\\dxvk.conf", exeDir);
    char v[64];
    if (!ConfValue(dxvk, "d3d9.shaderModel", v, sizeof(v)))
        Warn("dxvk.conf: d3d9.shaderModel not set (default 3) - set d3d9.shaderModel = 0, see README");
    else if (atoi(v) != 0) {
        char t[160]; snprintf(t, sizeof(t), "dxvk.conf: d3d9.shaderModel = %s - NFSU2 under Remix needs 0 (fixed function)", v); Warn(t);
    }
    const BYTE* base = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE))
        Warn("SPEED2.EXE is not large address aware - the Remix bridge and the full map need it (4GB patch)");
    if (g_cfg.profile != PROFILE_REMIX) Warn("RTX Remix found but the profile is Vanilla - switch Profile to Remix");
}
} // namespace

void DiagInstall(HMODULE)
{
    DetectRemix();
    CheckRemixSetup();
    if (g_cfg.crashLog) { g_prevFilter = SetUnhandledExceptionFilter(CrashFilter); AddVectoredExceptionHandler(1, StackOverflowHandler); }
    if (g_cfg.hangWatchdog > 0) { HANDLE t = CreateThread(nullptr, 0, HangThread, nullptr, 0, nullptr); if (t) CloseHandle(t); }
}

void DiagFrame() {}

const char* DiagRemixStatus() { return g_remixText; }
bool DiagRemixPresent() { return g_remix; }
int DiagWarningCount() { return g_warnCount; }
const char* DiagWarning(int i) { return i >= 0 && i < g_warnCount ? g_warn[i] : ""; }
