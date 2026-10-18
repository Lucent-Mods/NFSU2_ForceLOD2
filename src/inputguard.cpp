// InputGuard - see inputguard.h.
//
// The problem it solves. Blanking the game's DirectInput devices does nothing on a normal modded
// install, because NFSU_XtendedInput.asi has taken the controls over: its INI's KeyboardReadingMode
// picks GetAsyncKeyState (0), GetKeyboardState (1) or GetRawInputData (2), and none of those is
// DirectInput. So while the console or an HTML page had the keyboard, the car kept driving.
//
// How it stops that: not by detouring the Windows functions - that would hit every plugin and the
// game itself, and four of our plugins would end up stacked on the same bytes. Instead it rewrites
// the import table of the input plugin alone. Only that module's calls land in the stubs below;
// the game, ReShade, the other ASIs and our own hotkeys are untouched, and nothing is patched
// inside somebody else's code.
//
// Several of our plugins run together, so the block itself lives in shared memory, one slot each,
// each with a heartbeat. Any slot blocking blocks the input; a plugin that dies stops its heartbeat
// and its block expires after half a second instead of leaving the keyboard dead.
#include "inputguard.h"

#include <stdio.h>
#include <string.h>

namespace InputGuard {
namespace {

constexpr char kSharedName[] = "Local\\NFSU2_InputGuard";
constexpr DWORD kHeartbeatMs = 500;     // a block older than this belongs to a plugin that is gone
constexpr int   kSlots = 8;

struct Slot {
    volatile LONG owner;        // the plugin's module base, 0 for a free slot
    volatile LONG blocked;
    volatile LONG tick;         // GetTickCount of the last Tick() call
};
struct Shared {
    volatile LONG magic;        // 'NIG1' once the first plugin has set it up
    Slot slots[kSlots];
};
constexpr LONG kMagic = 0x3147494E;

Shared*  g_shared = nullptr;
Slot*    g_mine   = nullptr;
HMODULE  g_owner  = nullptr;
bool     g_installed = false;
int      g_guarded = 0;                 // modules whose imports we redirected
char     g_status[160] = "not installed";

// The real functions, resolved once. The stubs call these when nothing is blocking - never the
// previous value of the import slot, which may be another of our plugins' stub. Every stub asks
// the same shared state anyway, so going straight to Windows is both correct and shorter.
decltype(&GetAsyncKeyState)  g_realAsync = nullptr;
decltype(&GetKeyState)       g_realKey = nullptr;
decltype(&GetKeyboardState)  g_realKeyboard = nullptr;
decltype(&GetRawInputData)   g_realRaw = nullptr;

// XInput without <Xinput.h> or its import library: only the layout of XINPUT_STATE is needed.
struct PadState {
    DWORD packet;
    WORD  buttons; BYTE leftTrigger, rightTrigger; SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
typedef DWORD (WINAPI* XInputGetState_t)(DWORD user, PadState* state);
XInputGetState_t g_realXInput = nullptr;

bool AnyoneBlocking()
{
    if (!g_shared) return false;
    DWORD now = GetTickCount();
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = g_shared->slots[i];
        if (s.owner && s.blocked && now - static_cast<DWORD>(s.tick) < kHeartbeatMs) return true;
    }
    return false;
}

// ------------------------------------------------------------------ stubs ---
SHORT WINAPI AsyncKeyStateStub(int vk)
{
    if (AnyoneBlocking()) return 0;
    return g_realAsync(vk);
}

SHORT WINAPI KeyStateStub(int vk)
{
    if (AnyoneBlocking()) return 0;
    return g_realKey(vk);
}

BOOL WINAPI KeyboardStateStub(PBYTE state)
{
    if (!AnyoneBlocking()) return g_realKeyboard(state);
    if (state) memset(state, 0, 256);         // the caller still gets a valid, empty table
    return TRUE;
}

UINT WINAPI RawInputDataStub(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize)
{
    UINT r = g_realRaw(input, command, data, size, headerSize);
    // Only the data itself is emptied, never the size query: the caller checks the size it gets
    // back against what it asked for and complains if that changes.
    if (r != static_cast<UINT>(-1) && data && command == RID_INPUT && AnyoneBlocking()) {
        RAWINPUT* ri = static_cast<RAWINPUT*>(data);
        if (ri->header.dwType == RIM_TYPEKEYBOARD) {
            ri->data.keyboard.MakeCode = 0;
            ri->data.keyboard.VKey = 0xFF;                  // "no key", as Windows sends for fake events
            ri->data.keyboard.Flags = RI_KEY_BREAK;         // and released, not held
            ri->data.keyboard.Message = WM_NULL;
        } else if (ri->header.dwType == RIM_TYPEMOUSE) {
            ri->data.mouse.usButtonFlags = 0;
            ri->data.mouse.usButtonData = 0;
            ri->data.mouse.lLastX = 0;
            ri->data.mouse.lLastY = 0;
        }
    }
    return r;
}

// The pad stays "connected" (the input plugin keeps its device type and port mapping), it just reports
// nothing held: sticks centred, triggers and buttons released - a state poll, so nothing hangs afterwards.
DWORD WINAPI XInputGetStateStub(DWORD user, PadState* state)
{
    if (!g_realXInput) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = g_realXInput(user, state);
    if (r == ERROR_SUCCESS && state && AnyoneBlocking())
        memset(reinterpret_cast<BYTE*>(state) + sizeof(DWORD), 0, sizeof(PadState) - sizeof(DWORD));
    return r;
}

// NFSU_XtendedInput imports XINPUT1_4.dll by ordinal only (#2). The ordinal of XInputGetState differs between
// the DLL versions (#2 in xinput1_3 / 1_4, #4 in xinput9_1_0), so ask the loaded DLL which ordinal it is.
bool IsXInputGetState(const char* dll, const IMAGE_THUNK_DATA* name, BYTE* base)
{
    HMODULE x = GetModuleHandleA(dll);
    if (!x) return false;
    FARPROC wanted = GetProcAddress(x, "XInputGetState");
    if (!wanted) return false;
    bool match = IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)
        ? GetProcAddress(x, MAKEINTRESOURCEA(IMAGE_ORDINAL(name->u1.Ordinal))) == wanted
        : strcmp(reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name), "XInputGetState") == 0;
    if (match && !g_realXInput) g_realXInput = reinterpret_cast<XInputGetState_t>(wanted);
    return match && g_realXInput == reinterpret_cast<XInputGetState_t>(wanted);
}

// --------------------------------------------------------- the import table ---
// Redirects one module's imports of the named functions. Returns how many entries were rewritten.
int PatchImports(HMODULE mod)
{
    auto base = reinterpret_cast<BYTE*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;

    struct { const char* name; void* stub; } kWanted[] = {
        { "GetAsyncKeyState", &AsyncKeyStateStub },
        { "GetKeyState",      &KeyStateStub },
        { "GetKeyboardState", &KeyboardStateStub },
        { "GetRawInputData",  &RawInputDataStub },
    };

    int patched = 0;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        const bool user32 = _stricmp(dll, "USER32.dll") == 0 || _stricmp(dll, "USER32") == 0;
        const bool xinput = _strnicmp(dll, "XINPUT", 6) == 0;
        if (!user32 && !xinput) continue;
        if (!imp->OriginalFirstThunk) continue;       // a bound-only table: nothing to match names against
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            void* stub = nullptr;
            if (xinput) {
                if (IsXInputGetState(dll, names, base)) stub = reinterpret_cast<void*>(&XInputGetStateStub);
            } else if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                for (const auto& want : kWanted)
                    if (strcmp(reinterpret_cast<const char*>(byName->Name), want.name) == 0) { stub = want.stub; break; }
            }
            if (!stub) continue;
            void** entry = reinterpret_cast<void**>(&slots->u1.Function);
            if (*entry == stub) { ++patched; continue; }      // already ours (a second plugin)
            DWORD old = 0;
            if (VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) {
                *entry = stub;
                VirtualProtect(entry, sizeof(void*), old, &old);
                ++patched;
            }
        }
    }
    return patched;
}

bool NameMatches(const char* moduleName, const char* list)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    for (char* part = strtok(buf, ","); part; part = strtok(nullptr, ",")) {
        while (*part == ' ') ++part;
        size_t n = strlen(part);
        while (n && (part[n - 1] == ' ' || part[n - 1] == '\r' || part[n - 1] == '\n')) part[--n] = 0;
        if (!n) continue;
        // A name matches anywhere in the file name, so "XtendedInput" finds NFSU_XtendedInput.asi.
        for (const char* p = moduleName; *p; ++p)
            if (_strnicmp(p, part, n) == 0) return true;
    }
    return false;
}

void ClaimSlot()
{
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), kSharedName);
    if (!map) return;
    g_shared = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!g_shared) { CloseHandle(map); return; }       // the handle stays open for the process's life
    InterlockedCompareExchange(&g_shared->magic, kMagic, 0);

    LONG me = static_cast<LONG>(reinterpret_cast<intptr_t>(g_owner));
    DWORD now = GetTickCount();
    for (int i = 0; i < kSlots && !g_mine; ++i) {
        Slot& s = g_shared->slots[i];
        if (s.owner == me) { g_mine = &s; break; }                       // ours from an earlier load
        if (InterlockedCompareExchange(&s.owner, me, 0) == 0) g_mine = &s;
        // A slot whose plugin is long gone is taken over rather than leaked.
        else if (s.blocked == 0 && now - static_cast<DWORD>(s.tick) > 60000 &&
                 InterlockedCompareExchange(&s.owner, me, s.owner) == s.owner) g_mine = &s;
    }
    if (g_mine) { g_mine->blocked = 0; g_mine->tick = static_cast<LONG>(now); }
}

// The loaded modules, without linking psapi: kernel32 has carried this since Windows 7.
bool EnumModules(HMODULE* out, DWORD bytes, DWORD* needed)
{
    typedef BOOL(WINAPI * Enum_t)(HANDLE, HMODULE*, DWORD, LPDWORD);
    static Enum_t fn = reinterpret_cast<Enum_t>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules"));
    return fn && fn(GetCurrentProcess(), out, bytes, needed);
}

// Looks for the modules to guard and redirects their imports. Runs again later if none were there
// yet - an input plugin may well be loaded after us.
int ScanAndPatch(const char* list)
{
    int guarded = 0;
    char found[96] = "";
    HMODULE mods[512];
    DWORD needed = 0;
    if (EnumModules(mods, sizeof(mods), &needed)) {
        int count = static_cast<int>(needed / sizeof(HMODULE));
        if (count > 512) count = 512;
        for (int i = 0; i < count; ++i) {
            if (mods[i] == g_owner) continue;
            char path[MAX_PATH] = "";
            if (!GetModuleFileNameA(mods[i], path, MAX_PATH)) continue;
            const char* name = strrchr(path, '\\');
            name = name ? name + 1 : path;
            if (!NameMatches(name, list)) continue;
            int n = PatchImports(mods[i]);
            if (n > 0) {
                ++guarded;
                if (!found[0]) snprintf(found, sizeof(found), "%s (%d)", name, n);
            }
        }
    }
    if (guarded) snprintf(g_status, sizeof(g_status), "guarding %s", found);
    else         snprintf(g_status, sizeof(g_status), "nothing to guard (%s not loaded)", list);
    return guarded;
}

char g_list[256] = "NFSU_XtendedInput";

} // namespace

void Install(HMODULE owner, const char* moduleList)
{
    if (g_installed) return;
    g_installed = true;
    g_owner = owner;

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        g_realAsync    = reinterpret_cast<decltype(g_realAsync)>(GetProcAddress(user32, "GetAsyncKeyState"));
        g_realKey      = reinterpret_cast<decltype(g_realKey)>(GetProcAddress(user32, "GetKeyState"));
        g_realKeyboard = reinterpret_cast<decltype(g_realKeyboard)>(GetProcAddress(user32, "GetKeyboardState"));
        g_realRaw      = reinterpret_cast<decltype(g_realRaw)>(GetProcAddress(user32, "GetRawInputData"));
    }
    if (!g_realAsync || !g_realKey || !g_realKeyboard || !g_realRaw) {
        snprintf(g_status, sizeof(g_status), "user32 not resolved - guard off");
        return;
    }

    ClaimSlot();

    if (moduleList && *moduleList) snprintf(g_list, sizeof(g_list), "%s", moduleList);
    g_guarded = ScanAndPatch(g_list);
}

void Tick(bool blocked)
{
    // An input plugin loaded after us still gets guarded: look again now and then, for the first
    // half minute, until something is found.
    if (g_installed && !g_guarded) {
        static DWORD lastScan = 0, firstScan = 0;
        DWORD now = GetTickCount();
        if (!firstScan) firstScan = now;
        if (now - firstScan < 30000 && now - lastScan > 2000) {
            lastScan = now;
            g_guarded = ScanAndPatch(g_list);
        }
    }
    if (!g_mine) return;
    g_mine->blocked = blocked ? 1 : 0;
    g_mine->tick = static_cast<LONG>(GetTickCount());
}

bool Blocked() { return AnyoneBlocking(); }

// ------------------------------------------------- buffered DirectInput data ---
namespace {

// What a device currently reports as held down. The identity of an entry is its offset: for a
// plain keyboard that is the scan code, for the action-mapped keyboard NFSU2 uses it is the
// action's semantic, and the action id travels alongside in uAppData.
struct Held { DWORD ofs, appData; };
struct DeviceState {
    const void* device;
    Held  held[48];
    int   count;
    DWORD sequence;
};
DeviceState g_devices[8];

DeviceState* StateFor(const void* device)
{
    for (auto& d : g_devices) if (d.device == device) return &d;
    for (auto& d : g_devices) if (!d.device) { d.device = device; d.count = 0; d.sequence = 0; return &d; }
    return nullptr;                       // more devices than we track: they simply stay unfiltered
}

void Remember(DeviceState* s, DWORD ofs, DWORD appData, bool down)
{
    for (int i = 0; i < s->count; ++i) {
        if (s->held[i].ofs != ofs) continue;
        if (!down) s->held[i] = s->held[--s->count];         // released: drop it
        else s->held[i].appData = appData;
        return;
    }
    if (down && s->count < static_cast<int>(sizeof(s->held) / sizeof(s->held[0])))
        s->held[s->count++] = { ofs, appData };
}

} // namespace

unsigned FilterBufferedData(void* entries, unsigned stride, unsigned capacity, unsigned count,
                            bool blocked, const void* device)
{
    DeviceState* s = StateFor(device);
    if (!s || stride < 8) return count;
    BYTE* buf = static_cast<BYTE*>(entries);

    if (!blocked) {
        // Pass everything through, but keep track of what is held so we can let go of it later.
        for (unsigned i = 0; i < count && entries; ++i) {
            const BYTE* e = buf + static_cast<size_t>(i) * stride;
            DWORD ofs = *reinterpret_cast<const DWORD*>(e);
            DWORD data = *reinterpret_cast<const DWORD*>(e + 4);
            DWORD app = stride >= 20 ? *reinterpret_cast<const DWORD*>(e + 16) : 0;
            if (stride >= 16) s->sequence = *reinterpret_cast<const DWORD*>(e + 12);
            Remember(s, ofs, app, data != 0);
        }
        return count;
    }

    // Blocked: hand out the releases first, as many as fit, then stay silent.
    if (!entries || capacity == 0 || s->count == 0) { s->count = 0; return 0; }
    unsigned n = static_cast<unsigned>(s->count);
    if (n > capacity) n = capacity;
    DWORD now = GetTickCount();
    for (unsigned i = 0; i < n; ++i) {
        BYTE* e = buf + static_cast<size_t>(i) * stride;
        *reinterpret_cast<DWORD*>(e) = s->held[i].ofs;
        *reinterpret_cast<DWORD*>(e + 4) = 0;                         // released
        if (stride >= 12) *reinterpret_cast<DWORD*>(e + 8) = now;
        if (stride >= 16) *reinterpret_cast<DWORD*>(e + 12) = ++s->sequence;
        if (stride >= 20) *reinterpret_cast<DWORD*>(e + 16) = s->held[i].appData;
    }
    // Whatever did not fit goes out on the next call.
    for (unsigned i = n; i < static_cast<unsigned>(s->count); ++i) s->held[i - n] = s->held[i];
    s->count -= static_cast<int>(n);
    return n;
}

const char* Status()
{
    static char line[240];
    snprintf(line, sizeof(line), "%s, %s", g_status, AnyoneBlocking() ? "blocking now" : "idle");
    return line;
}

} // namespace InputGuard
