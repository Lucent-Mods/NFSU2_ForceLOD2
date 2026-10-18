// Device hooks + menu.
//
// Entry: the game's own import of Direct3DCreate9 (whatever d3d9.dll is loaded - system or the RTX Remix
// bridge) -> IDirect3D9::CreateDevice (vtable slot 16) -> MinHook on EndScene (42) and Reset (16). No draw
// call hooks: nothing runs per draw. EndScene also drives the once-per-frame work of the modules.
// The menu draws pre-transformed vertices (patched ImGui DX9 backend), which Remix treats as UI.
// DirectInput: the game's keyboard comes from EnumDevicesBySemantics, so that is wrapped as well; while the
// menu is open keyboard and mouse are blanked and InputGuard holds back NFSU_XtendedInput.
#include "core.h"
#include "inputguard.h"
#include <d3d9.h>
#include <dinput.h>
#include <cstdio>
#include <intrin.h>
#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(APIENTRY* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(APIENTRY* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(APIENTRY* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* DICreateDevice_t)(void*, REFGUID, void**, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* DIGetDeviceState_t)(void*, DWORD, LPVOID);
typedef HRESULT(STDMETHODCALLTYPE* DIGetDeviceData_t)(void*, DWORD, void*, LPDWORD, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* DIEnumBySemantics_t)(void*, LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID, DWORD);

Direct3DCreate9_t  g_origD3DCreate   = nullptr;
CreateDevice_t     g_origCreateDevice = nullptr;
EndScene_t         g_origEndScene    = nullptr;
Reset_t            g_origReset       = nullptr;
typedef HRESULT(APIENTRY* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(APIENTRY* DIP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(APIENTRY* DP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
Present_t g_origPresent = nullptr; DIP_t g_origDIP = nullptr; DP_t g_origDP = nullptr;
DI8Create_t        g_origDI8Create   = nullptr;
DICreateDevice_t   g_origDICreate    = nullptr;
DIGetDeviceState_t g_origGetState    = nullptr;
DIGetDeviceData_t  g_origGetData     = nullptr;
DIEnumBySemantics_t g_origEnumSem    = nullptr;
LPDIENUMDEVICESBYSEMANTICSCBA g_gameSemCb = nullptr; void* g_gameSemRef = nullptr;

HMODULE g_self = nullptr;
bool    g_deviceHooked = false, g_imgui = false, g_visible = false, g_keyWas = false;
HWND    g_hwnd = nullptr; WNDPROC g_origWndProc = nullptr;
void*   g_blocked[8]; int g_blockedCount = 0;
bool    g_dikDown = false, g_dikSeen = false;
float   g_fps = 0.f; DWORD g_fpsTick = 0; int g_fpsFrames = 0;
UINT    g_bbWidth = 0, g_bbHeight = 0;
bool    g_dirty = false;   // settings changed since the last save

bool MenuOwnsInput() { return g_visible && g_imgui; }
bool IsBlocked(void* d) { for (int i = 0; i < g_blockedCount; ++i) if (g_blocked[i] == d) return true; return false; }

void* SwapVtable(void* obj, int index, void* repl)
{
    void** slot = *reinterpret_cast<void***>(obj) + index;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void* prev = *slot; *slot = repl;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return prev;
}

void* PatchImport(const char* dll, const char* func, void* repl)
{
    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    IMAGE_NT_HEADERS* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* funcs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++funcs) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(byName->Name, func) != 0) continue;
            DWORD old;
            if (!VirtualProtect(&funcs->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old)) return nullptr;
            void* prev = reinterpret_cast<void*>(funcs->u1.Function);
            funcs->u1.Function = reinterpret_cast<DWORD>(repl);
            VirtualProtect(&funcs->u1.Function, sizeof(DWORD), old, &old);
            return prev;
        }
    }
    return nullptr;
}

// --- DirectInput -----------------------------------------------------------------------------
int VkToDik(int vk)
{
    switch (vk) {
    case VK_INSERT: return 0xD2; case VK_END: return 0xCF; case VK_HOME: return 0xC7; case VK_DELETE: return 0xD3;
    case VK_PRIOR: return 0xC9; case VK_NEXT: return 0xD1; case VK_PAUSE: return 0xC5; case VK_SCROLL: return 0x46;
    case VK_F1: return 0x3B; case VK_F2: return 0x3C; case VK_F3: return 0x3D; case VK_F4: return 0x3E; case VK_F5: return 0x3F;
    case VK_F6: return 0x40; case VK_F7: return 0x41; case VK_F8: return 0x42; case VK_F9: return 0x43; case VK_F10: return 0x44;
    case VK_F11: return 0x57; case VK_F12: return 0x58; case VK_OEM_3: return 0x29;
    default: return 0;
    }
}

HRESULT STDMETHODCALLTYPE GetStateHook(void* self, DWORD cb, LPVOID data)
{
    HRESULT hr = g_origGetState(self, cb, data);
    if (SUCCEEDED(hr) && data && cb == 256 && IsBlocked(self)) {
        int dik = VkToDik(g_cfg.menuKey);
        if (dik) { g_dikSeen = true; g_dikDown = (static_cast<const BYTE*>(data)[dik] & 0x80) != 0; }
    }
    if (MenuOwnsInput() && IsBlocked(self) && data && cb) { memset(data, 0, cb); return DI_OK; }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDataHook(void* self, DWORD cbObj, void* rgdod, LPDWORD inOut, DWORD flags)
{
    DWORD capacity = inOut ? *inOut : 0;
    HRESULT hr = g_origGetData(self, cbObj, rgdod, inOut, flags);
    if (SUCCEEDED(hr) && rgdod && inOut && IsBlocked(self) && cbObj >= 8) {
        int dik = VkToDik(g_cfg.menuKey);
        const BYTE* e = static_cast<const BYTE*>(rgdod);
        for (DWORD i = 0; dik && i < *inOut; ++i, e += cbObj)
            if (static_cast<int>(*reinterpret_cast<const DWORD*>(e)) == dik) { g_dikSeen = true; g_dikDown = (*reinterpret_cast<const DWORD*>(e + 4) & 0x80) != 0; }
    }
    if (SUCCEEDED(hr) && inOut && IsBlocked(self) && !(flags & DIGDD_PEEK))
        *inOut = InputGuard::FilterBufferedData(rgdod, cbObj, capacity, *inOut, MenuOwnsInput(), self);
    return hr;
}

// The value a vtable slot had when its module was loaded: read from the module's file (plus relocation). Another
// plugin may have swapped the slot already - its hook is not what we want to wrap.
void* PristineSlot(void* const* slot)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(slot), &mod) || !mod) return nullptr;
    const BYTE* base = reinterpret_cast<const BYTE*>(mod);
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    DWORD rva = static_cast<DWORD>(reinterpret_cast<const BYTE*>(slot) - base);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    DWORD fileOff = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (rva >= sec->VirtualAddress && rva < sec->VirtualAddress + sec->SizeOfRawData) { fileOff = rva - sec->VirtualAddress + sec->PointerToRawData; break; }
    if (!fileOff) return nullptr;
    char path[MAX_PATH]; GetModuleFileNameA(mod, path, MAX_PATH);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    DWORD value = 0, got = 0;
    SetFilePointer(f, fileOff, nullptr, FILE_BEGIN);
    ReadFile(f, &value, 4, &got, nullptr);
    CloseHandle(f);
    if (got != 4) return nullptr;
    value = value - nt->OptionalHeader.ImageBase + reinterpret_cast<DWORD>(base);
    if (value < reinterpret_cast<DWORD>(base) || value >= reinterpret_cast<DWORD>(base) + nt->OptionalHeader.SizeOfImage) return nullptr;
    return reinterpret_cast<void*>(value);
}

// One inline hook on the real GetDeviceState / GetDeviceData (shared by every device), installed with the first
// keyboard or mouse. Other plugins may swap the device vtable as they like - they end up in the real function, which
// is ours now; no chain of vtable hooks that point at each other (the LiveTextures stack overflow, 2026-10-17).
const char* ModuleOf(const void* p, char* buf, size_t len)
{
    HMODULE m = nullptr; char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(p), &m) && m) {
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* s = strrchr(path, 0x5C);
        snprintf(buf, len, "%s+0x%X", s ? s + 1 : path, static_cast<unsigned>(reinterpret_cast<DWORD>(p) - reinterpret_cast<DWORD>(m)));
    } else snprintf(buf, len, "no module");
    return buf;
}

bool g_diHooked = false;
void TakeDevice(void* dev)
{
    if (!dev) return;
    if (!IsBlocked(dev)) {
        if (g_blockedCount < 8) g_blocked[g_blockedCount++] = dev;
        else { memmove(g_blocked, g_blocked + 1, 7 * sizeof(void*)); g_blocked[7] = dev; }   // keep the newest
    }
    if (g_diHooked) return;
    g_diHooked = true;
    void* const* vt = *reinterpret_cast<void* const* const*>(dev);
    // pristine value from the file if the vtable lives in a module image; otherwise (vtable built at run time by a
    // wrapper) the function the slot points to now - an inline hook on it is just as loop-free
    void* realState = PristineSlot(vt + 9);
    void* realData  = PristineSlot(vt + 10);
    char modS[160], modV[160];
    if (!realState || !realData) {
        realState = vt[9]; realData = vt[10];
        Log("[INFO] input: device vtable %p is not in a module file (%s) - hooking the current targets", vt, ModuleOf(vt, modV, sizeof(modV)));
    }
    Log("input: GetDeviceState -> %s", ModuleOf(realState, modS, sizeof(modS)));
    if (MH_CreateHook(realState, reinterpret_cast<void*>(&GetStateHook), reinterpret_cast<void**>(&g_origGetState)) != MH_OK ||
        MH_CreateHook(realData, reinterpret_cast<void*>(&GetDataHook), reinterpret_cast<void**>(&g_origGetData)) != MH_OK ||
        MH_EnableHook(realState) != MH_OK || MH_EnableHook(realData) != MH_OK) {
        Log("[FAIL] input: hooking GetDeviceState/GetDeviceData failed - menu input is not blocked"); return;
    }
    Log("[OK] input: GetDeviceState %p / GetDeviceData %p hooked inline (blocked while the menu is open)", realState, realData);
}

HRESULT STDMETHODCALLTYPE DICreateHook(void* self, REFGUID guid, void** out, LPUNKNOWN outer)
{
    HRESULT hr = g_origDICreate(self, guid, out, outer);
    if (FAILED(hr) || !out || !*out) return hr;
    static const GUID kKbd   = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    static const GUID kMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
    if (IsEqualGUID(guid, kKbd) || IsEqualGUID(guid, kMouse)) TakeDevice(*out);
    return hr;
}

BOOL FAR PASCAL SemCallback(LPCDIDEVICEINSTANCEA inst, LPDIRECTINPUTDEVICE8A dev, DWORD flags, DWORD remaining, LPVOID)
{
    if (dev && inst) {
        DWORD type = GET_DIDEVICE_TYPE(inst->dwDevType);
        if (type == DI8DEVTYPE_KEYBOARD || type == DI8DEVTYPE_MOUSE) TakeDevice(dev);
    }
    return g_gameSemCb ? g_gameSemCb(inst, dev, flags, remaining, g_gameSemRef) : DIENUM_CONTINUE;
}

HRESULT STDMETHODCALLTYPE EnumSemHook(void* self, LPCSTR user, LPDIACTIONFORMATA fmt, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD flags)
{
    if (!cb) return g_origEnumSem(self, user, fmt, cb, ref, flags);
    LPDIENUMDEVICESBYSEMANTICSCBA prevCb = g_gameSemCb; void* prevRef = g_gameSemRef;
    g_gameSemCb = cb; g_gameSemRef = ref;
    HRESULT hr = g_origEnumSem(self, user, fmt, &SemCallback, nullptr, flags);
    g_gameSemCb = prevCb; g_gameSemRef = prevRef;
    return hr;
}

HRESULT WINAPI DI8CreateHook(HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HRESULT hr = g_origDI8Create(h, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !g_origDICreate) {
        g_origDICreate = reinterpret_cast<DICreateDevice_t>(SwapVtable(*out, 3, reinterpret_cast<void*>(&DICreateHook)));
        g_origEnumSem  = reinterpret_cast<DIEnumBySemantics_t>(SwapVtable(*out, 9, reinterpret_cast<void*>(&EnumSemHook)));
    }
    return hr;
}

// --- window / ImGui ---------------------------------------------------------------------------
LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (MenuOwnsInput()) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        switch (msg) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            return 0;
        }
    }
    return CallWindowProc(g_origWndProc, h, msg, w, l);
}

void InitImGui(IDirect3DDevice9* dev)
{
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    dev->GetCreationParameters(&cp);
    g_hwnd = cp.hFocusWindow;
    if (!g_hwnd) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDrawCursor = true;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.f; st.FrameRounding = 4.f; st.GrabRounding = 4.f; st.TabRounding = 4.f;
    st.Colors[ImGuiCol_WindowBg].w = 0.94f;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(dev);
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcHook)));
    g_imgui = true;
    Log("menu: ImGui ready (hwnd %p)", g_hwnd);
}

// --- menu ------------------------------------------------------------------------------------
const ImVec4 kGreen(0.35f, 0.85f, 0.35f, 1.f), kRed(0.95f, 0.35f, 0.3f, 1.f), kYellow(0.95f, 0.8f, 0.3f, 1.f), kGrey(0.6f, 0.6f, 0.6f, 1.f);

void Help(const char* text)
{
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}
void Restart() { ImGui::SameLine(); ImGui::TextColored(kYellow, "(restart)"); }

bool SliderI(const char* label, int* v, int lo, int hi, const char* help)
{
    bool ch = ImGui::SliderInt(label, v, lo, hi);
    if (help) Help(help);
    if (ch) g_dirty = true;
    return ch;
}
bool Check(const char* label, int* v, const char* help)
{
    bool b = *v != 0;
    bool ch = ImGui::Checkbox(label, &b);
    if (ch) { *v = b ? 1 : 0; g_dirty = true; }
    if (help) Help(help);
    return ch;
}

void CounterRow(const char* name, unsigned v, bool mustBeZero, const char* help)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(name); if (help) Help(help);
    ImGui::TableNextColumn();
    if (mustBeZero) ImGui::TextColored(v ? kRed : kGreen, "%u", v); else ImGui::Text("%u", v);
}

void ApplyAllLive() { LodApplyLive(); SettingsApplyLive(); DespawnApplyLive(); FullMapApplyLive(); }

// keys the DirectInput keyboard path knows (VkToDik) - any other key would only work through GetAsyncKeyState
const int   kMenuKeys[]     = { VK_INSERT, VK_HOME, VK_END, VK_DELETE, VK_PRIOR, VK_NEXT, VK_PAUSE, VK_SCROLL, VK_OEM_3,
                                VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12 };
const char* kMenuKeyNames[] = { "Insert", "Home", "End", "Delete", "Page Up", "Page Down", "Pause", "Scroll Lock", "` (tilde)",
                                "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
const int   kMenuKeyCount   = sizeof(kMenuKeys) / sizeof(kMenuKeys[0]);

void TabStatus()
{
    const char* profiles[2] = { "Remix", "Vanilla" };
    int p = g_cfg.profile;
    if (ImGui::Combo("Profile", &p, profiles, 2) && p != g_cfg.profile) {
        ConfigSwitchProfile(p);
        ApplyAllLive(); g_dirty = true;
    }
    Help("Remix: LOD A everywhere, no reflection / mirror / headlight views, no per-frame decals and post quads - stable geometry hashes.\n"
         "Vanilla: LOD A everywhere, the game's views and effects stay as in the options menu.\n"
         "Switching applies every live value at once; pool and render-target values need a restart.");
    ImGui::SameLine();
    if (ImGui::Button("Reset to profile values")) { ConfigDefaults(g_cfg, g_cfg.profile); ApplyAllLive(); g_dirty = true; }
    Help("Every value back to the start value of the current profile (engine settings too). Menu key and log stay. Save to keep it.");
    ImGui::TextColored(DiagRemixPresent() ? kGreen : kGrey, "%s", DiagRemixStatus());
    for (int i = 0; i < DiagWarningCount(); ++i) ImGui::TextColored(kYellow, "! %s", DiagWarning(i));
    PoolInfo pi = PoolsInfo();
    ImGui::Text("SPEED2.EXE LAA: %s   |   back buffer %ux%u   |   %.0f fps", pi.exeLaa ? "yes" : "no", g_bbWidth, g_bbHeight, g_fps);
    ImGui::TextColored(LodHooked() ? kGreen : kRed, LodHooked() ? "World LOD A hook active" : "World LOD A hook NOT active (site owned by another plugin?)");
    ImGui::Separator();
    LodCounters c = LodLastFrame();
    ImGui::TextUnformatted("Last frame (red = breaks the Remix rule \"one mesh per object\")");
    if (ImGui::BeginTable("cnt", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Counter", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80.f);
        CounterRow("Scenery drawn as LOD A", c.drawnA, false, nullptr);
        CounterRow("Scenery without LOD A in its data (own only mesh)", c.drawnNoA, false, "These objects have no LOD A at all - they always draw the same B/C mesh, so their hash is stable too.");
        CounterRow("Reflection views using the game's B/C choice", c.refBC, true, "ReflectionLodA is off and a reflection view runs.");
        CounterRow("LOD A solid not in memory -> nothing drawn", c.unboundCulled, true, "Its solid is not in memory at all (pack not loaded) - Vanilla would draw LOD B here (a different hash). LOD A is bound on the spot whenever its solid exists; the streamer keeps LOD A units loaded.");
        CounterRow("LOD A bound on the spot this frame", c.bound, false, nullptr);
        CounterRow("Bind failed: solid not in memory", c.bindMissing, true, nullptr);
        CounterRow("Below the pixel limit -> culled", c.pixelCulled, false, nullptr);
        CounterRow("Draw list full -> dropped by the game", c.listFull, true, "Raise Pools > Draw list (restart).");
        ImGui::EndTable();
    }
    FullMapStats fm = FullMapGet();
    ImGui::Text("Full map: region %s, camera zone %d, vanilla list %d, extra sections %d", FullMapRegion(), fm.zone, fm.listed, fm.extras);
    int body = *reinterpret_cast<const int*>(0x00802728), tyre = *reinterpret_cast<const int*>(0x0080272C);
    ImGui::TextColored(body == 0 && tyre == 0 ? kGreen : kYellow, "Car LOD override: body %d, tyres %d (0 = A, -1 = game)", body, tyre);
    ImGui::SeparatorText("Menu");
    int ki = 0;
    for (int i = 0; i < kMenuKeyCount; ++i) if (kMenuKeys[i] == g_cfg.menuKey) ki = i;
    if (ImGui::Combo("Menu key", &ki, kMenuKeyNames, kMenuKeyCount)) { g_cfg.menuKey = kMenuKeys[ki]; g_dirty = true; }
    Help("Key that opens and closes this menu. Takes effect at once; Save to keep it.");
}

void TabLod()
{
    ImGui::SeparatorText("World");
    if (SliderI("Min pixel size", &g_cfg.worldMinPx, 1, 32, "Scenery smaller than this on screen is not drawn. Game scale (512 px wide reference), vanilla 17.\nLower = more distant objects (CPU cost).")) LodApplyLive();
    if (SliderI("Pixel hysteresis", &g_cfg.worldPxHyst, 0, 8, "Band above the limit that keeps the previous decision, so objects at the edge do not flicker in and out.")) LodApplyLive();
    if (Check("LOD A in reflection views", &g_cfg.reflectionLodA, "Reflection / env views draw LOD A too (vanilla B/C). Remix profile turns those views off anyway.")) LodApplyLive();
    ImGui::SeparatorText("Cars");
    if (SliderI("Body LOD override", &g_cfg.carLod, -1, 3, "0 = LOD A always, -1 = the game picks by size.")) LodApplyLive();
    if (SliderI("Tyre LOD override", &g_cfg.carTyreLod, -1, 3, "0 = LOD A always, -1 = the game picks by size.")) LodApplyLive();
    if (Check("Cullable parts always drawn", &g_cfg.cullableParts, "Underbody, brakes, side parts are never hidden depending on the camera angle.")) LodApplyLive();
    if (SliderI("Car min pixel size", &g_cfg.carMinPx, 1, 16, "Cars smaller than this are not drawn (vanilla 4).")) LodApplyLive();
    if (SliderI("Car low-detail below px", &g_cfg.carLowDetailPx, 0, 64, "Below this size cars lose wheels, brakes and attachments (vanilla 18, 0 = never).")) LodApplyLive();
    if (Check("Respawn blink off", &g_cfg.carBlinkOff, "A respawning car is hidden every second frame in vanilla.")) LodApplyLive();
    if (Check("Precull BooBoo map off", &g_cfg.boobooOff, "The game switches occlusion culling off in some cells (PrecullerBooBooScript.hoo) - there everything is drawn. Off = culling always on.")) LodApplyLive();
}

void TabCulling()
{
    if (g_cfg.profile == PROFILE_REMIX) {
        ImGui::SeparatorText("Around the camera (Remix)");
        SliderI("Guard radius (m)", &g_cfg.guardRadius, 0, 300, "Objects within this distance of the camera are never culled - not beside, not behind the camera, not by occlusion or size. Remix keeps their lights and shadows. 0 = vanilla culling.");
        SliderI("No occlusion within (m)", &g_cfg.precullOffRadius, 0, 600, "The game's occlusion data (precull) is ignored within this distance. 0 = vanilla.");
    } else {
        ImGui::TextDisabled("Guard radius / occlusion radius: Remix profile only (Vanilla culls like the game).");
    }
    ImGui::SeparatorText("Despawn / flares");
    if (SliderI("Traffic / encounter despawn (m)", &g_cfg.trafficDespawn, 100, 1000, "Traffic cars are teleported away and encounter cars removed beyond this distance (vanilla 250).")) DespawnApplyLive();
    if (SliderI("Light flares up to (m)", &g_cfg.flareDistance, 100, 1000, "Street lamps, neon and other light flares fade out up to this distance (vanilla 200).")) DespawnApplyLive();
}

void TabMap()
{
    ImGui::SeparatorText("Streaming");
    Check("Full map", &g_cfg.fullMap, "Whole region loaded before the start; real sections everywhere instead of the coarse stand-ins."); Restart();
    Check("Draw extra sections", &g_cfg.fullMapExtras, "Also draw every other loaded real section (far geometry in LOD A instead of stand-ins). Costs a lot of frame time: those sections have no valid occlusion data. Off = whole map loaded, drawn like vanilla.");
    Check("Far sections with precull", &g_cfg.extraPrecull, "The far sections use the game's occlusion data too - 50-90 % fewer objects. Off = every far object drawn, very expensive.");
    if (g_cfg.profile == PROFILE_REMIX)
        Check("Capture mode (draw everything)", &g_cfg.captureAll, "For an RTX Remix capture: the whole region is streamed in and EVERY object is drawn - no occlusion table, no view frustum, no pixel limit - only the panoramas are left out. Very expensive. Off = Remix draws exactly like vanilla (LOD A).");
    if (Check("Replace city panoramas", &g_cfg.fullMapReplace, "City panoramas whose area is covered by real geometry are hidden and the real sections are drawn (LOD A). Terrain / ocean panoramas without real counterpart stay.")) FullMapApplyLive();
    if (SliderI("Far sections min pixel size", &g_cfg.extraMinPx, 0, 40, "Own pixel limit for the extra (far) sections. 0 = same as the world limit (everything).")) LodApplyLive();
    if (g_cfg.profile == PROFILE_VANILLA) {
        Check("Static batching of far sections", &g_cfg.staticBatching, "The far sections are merged into a few thousand static meshes in the loading screen - thousands of draw calls fewer. Built at load (switching on needs the next load), switching off is live. Never in the Remix profile (merged meshes have other hashes).");
        Check("Batch trees too", &g_cfg.batchTrees, "Swaying trees are merged as well; within 200 m they are drawn one by one and sway, farther away they stand still (nobody sees the sway there). Next load."); Restart();
        SliderI("Batch memory limit (MB)", &g_cfg.batchBudgetMB, 16, 512, "Upper limit for the merged buffers (VRAM, plus the same again in RAM as the managed copy). Next load."); Restart();
        BatchStats bs = BatchGet();
        ImGui::Text("Batches: %d meshes, %.1f MB, %d instances, built in %u ms | drawn %d, beyond distance %d, outside view %d, leftover %d",
                    bs.meshes, bs.mb, bs.batchedInstances, bs.buildMs, bs.emitted, bs.distCulled, bs.frustumCulled, bs.leftoverCalls);
        if (bs.deadMeshes || bs.fallbackSections) ImGui::TextColored(kYellow, "Dropped meshes %d, sections per instance %d (stale %d)", bs.deadMeshes, bs.fallbackSections, bs.staleSections);
    }
    Check("Keep LOD A units loaded", &g_cfg.lodaUnits, "Every shared LOD unit (X/Y < 50: LOD A meshes of most objects + their textures) stays loaded, so LOD A exists for anything drawn - also beyond the car's zone. +3 MB (L4RA) / +6 MB (L4RD)."); Restart();
}

void TabSettings()
{
    ImGui::TextWrapped("Forced engine settings. \"Game\" = the options menu decides. Applied live and re-applied whenever the options menu saves.");
    const char* names3[3] = { "Game", "Off", "On" };
    const char* names4[4] = { "Game", "Off", "Low", "High" };
    for (int i = 0; i < HS_Count; ++i) {
        int sel = g_cfg.hidden[i] + 1;
        bool ch = kHidden[i].maxValue == 2 ? ImGui::Combo(kHidden[i].key, &sel, names4, 4) : ImGui::Combo(kHidden[i].key, &sel, names3, 3);
        Help(kHidden[i].help);
        if (ch) { g_cfg.hidden[i] = sel - 1; g_dirty = true; SettingsApplyLive(); }
    }
    if (g_cfg.profile == PROFILE_VANILLA) {
        ImGui::SeparatorText("Reflection sizes (restart)");
        const char* road[3] = { "320x240 (vanilla)", "640x480", "1280x960" };
        int ri = g_cfg.roadReflScale == 4 ? 2 : (g_cfg.roadReflScale == 2 ? 1 : 0);
        if (ImGui::Combo("Road reflection", &ri, road, 3)) { g_cfg.roadReflScale = ri == 2 ? 4 : (ri == 1 ? 2 : 1); g_dirty = true; }
        const char* env[3] = { "128 (vanilla)", "256", "512" };
        int ei = g_cfg.carEnvMapSize == 512 ? 2 : (g_cfg.carEnvMapSize == 256 ? 1 : 0);
        if (ImGui::Combo("Car reflection cube", &ei, env, 3)) { g_cfg.carEnvMapSize = ei == 2 ? 512 : (ei == 1 ? 256 : 0); g_dirty = true; }
        const char* mir[3] = { "256 (vanilla)", "512", "1024" };
        int mi = g_cfg.mirrorSize == 1024 ? 2 : (g_cfg.mirrorSize == 512 ? 1 : 0);
        if (ImGui::Combo("Rear-view mirror", &mi, mir, 3)) { g_cfg.mirrorSize = mi == 2 ? 1024 : (mi == 1 ? 512 : 0); g_dirty = true; }
        ImGui::TextDisabled("Now: %s", ReflectionsStatus());
    }
    ImGui::SeparatorText("Views");
    ImGui::TextWrapped("For RTX Remix switch \"Car Reflection\" and \"Road Reflection\" off in the game's own display options - "
                       "the game then never creates those views. (Forcing them from here crashed the options menu.)");
}

void TabPools()
{
    ImGui::TextColored(kYellow, "All values on this page take effect after a restart.");
    SliderI("Streaming pool %", &g_cfg.streamPoolPct, 100, 200, "World streaming memory. Max 200: above that the city lights vanish after a shop visit.");
    SliderI("Car pack pool %", &g_cfg.packPoolPct, 100, 400, "Car loader memory. Too small = cars rebuilt under pressure.");
    SliderI("ePoly / FrameMalloc %", &g_cfg.polyBufPct, 100, 1000, "Per-frame matrices of every drawn object and car. Runs dry = objects and whole cars vanish for a frame.");
    SliderI("Front end pool %", &g_cfg.fengPoolPct, 100, 400, "Menus. Only for front-end heavy mods.");
    SliderI("Draw list %", &g_cfg.drawListPct, 100, 1000, "Scenery draw list, vanilla 4200 entries for all views. Full = objects dropped.");
    Check("LAA pointer fix", &g_cfg.laaFix, "Needed when SPEED2.EXE is large address aware (RTX Remix, big pools): pointer compares become unsigned.");
    Check("Streamer limits", &g_cfg.streamerLimits, "Stream heap gets 1024 extra block nodes (vanilla 192, taken without a check), wanted / deferred lists 256 / 64 -> 1024.\nNeeded for the full map, harmless without it.");
    PoolInfo pi = PoolsInfo();
    ImGui::Text("Draw list now: %u entries", pi.drawListEntries);
    ImGui::Text("Streamer limits: %s, stream heap free nodes: %d", pi.streamerLimits ? "on" : "off", StreamHeapFreeNodes());
    ImGui::SeparatorText("Crash fixes of the game");
    Check("Heap guard", &g_cfg.heapGuard, "Track unload crash: a freed block whose list neighbour is gone is logged and freed safely.");
    Check("Pool lock", &g_cfg.poolLock, "Multi-core crashes: allocation and free of the game's memory pools from two threads at once.");
    Check("File guard", &g_cfg.fileGuard, "Shop crash 0x6F6AF9: the file request list runs empty after quick shop visits.");
    Check("D3DX below 2 GB", &g_cfg.lowMemD3dx, "LAA only: the game's D3DX mistakes names above 2 GB for handles and crashes.");
    MemFixStats m = MemFixGet();
    ImGui::Text("Other-thread allocs %ld / frees %ld, heap guard hits %ld, file waits %ld refused %ld (min free %d)",
                m.otherThreadAllocs, m.otherThreadFrees, m.heapGuardHits, m.fileWaits, m.fileRefused, m.fileMinFree == 0x7FFFFFFF ? -1 : m.fileMinFree);
}

void TabDiag()
{
    ImGui::SeparatorText("Frame time (last 10 s of driving, same as the [perf] log line)");
    PerfSummary ps = PerfLast();
    if (!ps.valid) ImGui::TextDisabled("No data yet - measured only while driving or racing, every 10 s.");
    else {
        ImGui::Text("%d frames, avg %.2f ms (%.0f fps), max %.1f ms, %u s ago", ps.frames, ps.avgMs, ps.avgMs > 0 ? 1000.0 / ps.avgMs : 0.0, ps.maxMs, (GetTickCount() - ps.tick) / 1000);
        ImGui::Text("Draw calls avg %u, max %u | primitives avg %u | zone switches %d", ps.drawsAvg, ps.drawsMax, ps.primsAvg, ps.zoneSwitches);
        if (ImGui::BeginTable("perf", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.f);
            ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthFixed, 140.f);
            ImGui::TableHeadersRow();
            for (int s = 0; s <= PERF_STAGES; ++s) {
                const bool other = s == PERF_STAGES, nested = s >= 9 && !other;   // display .. present run inside render
                const double ms = other ? ps.otherMs : ps.stageMs[s];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s%s", nested ? "    " : "", other ? "other (input, AI, messages)" : PerfStageName(s));
                ImGui::TableNextColumn(); ImGui::Text("%.2f", ms);
                ImGui::TableNextColumn(); ImGui::ProgressBar(ps.avgMs > 0 ? static_cast<float>(ms / ps.avgMs) : 0.f, ImVec2(-1, 0), "");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Indented stages are part of render. present = waiting for the GPU / vsync.");
    }
    ImGui::SeparatorText("Patches");
    ImGui::TextWrapped("Every byte this plugin writes. Green = ours, blue = identical patch by another plugin, red = another plugin owns the site (left alone).");
    if (ImGui::BeginTable("patches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Patch", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableHeadersRow();
        static const char* stateName[5] = { "off", "on", "shared", "FOREIGN", "FAILED" };
        static const ImVec4 stateCol[5] = { kGrey, kGreen, ImVec4(0.4f, 0.7f, 1.f, 1.f), kRed, kRed };
        for (int i = 0; i < PatchCount(); ++i) {
            const Patch* p = PatchAt(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p->id);
            if (p->note[0] && ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(p->note); ImGui::EndTooltip(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p->module);
            ImGui::TableNextColumn(); ImGui::Text("0x%06X", p->addr);
            ImGui::TableNextColumn(); ImGui::TextColored(stateCol[p->state], "%s", stateName[p->state]);
        }
        ImGui::EndTable();
    }
}

void DrawMenu()
{
    ImGui::SetNextWindowSize(ImVec2(660, 600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("NFSU2 ForceLOD 2  " FL2_VERSION, &g_visible)) { ImGui::End(); return; }
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Status"))      { TabStatus();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("LOD"))         { TabLod();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Map"))         { TabMap();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Culling"))     { TabCulling();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings"))    { TabSettings(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Pools"))       { TabPools();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Diagnostics")) { TabDiag();     ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Save to INI")) { ConfigSave(); g_dirty = false; }
    ImGui::SameLine();
    if (g_dirty) ImGui::TextColored(kYellow, "unsaved changes"); else ImGui::TextDisabled("saved");
    ImGui::End();
}

// --- D3D9 ------------------------------------------------------------------------------------
// EndScene is called from RenderFrame (return 0x5D2B76), but also during device setup (0x5B9C02) and inside the
// static D3DX (0x6D9B6B, render-to-surface). Only the RenderFrame call is "the frame"; if a wrapper between the game
// and us hides the return address, after 300 calls without it every call counts.
bool g_seenFrameCall = false; int g_endSceneCalls = 0;

HRESULT APIENTRY EndSceneHook(IDirect3DDevice9* dev)
{
    DWORD ret = reinterpret_cast<DWORD>(_ReturnAddress());
    if (ret == 0x005D2B76) g_seenFrameCall = true;
    bool frameCall = ret == 0x005D2B76 || (!g_seenFrameCall && ++g_endSceneCalls > 300);
    if (!frameCall) return g_origEndScene(dev);
    struct EndSceneTimer { LARGE_INTEGER t0 = PerfNow(); ~EndSceneTimer() { PerfD3D(0, PerfMsSince(t0)); } } endSceneTimer;
    LodFrame();
    DiagFrame();
    MemFixFrame();
    ++g_fpsFrames;
    DWORD now = GetTickCount();
    if (now - g_fpsTick >= 500) { g_fps = g_fpsFrames * 1000.f / (now - g_fpsTick); g_fpsFrames = 0; g_fpsTick = now; }

    bool down = g_dikSeen ? g_dikDown : (GetAsyncKeyState(g_cfg.menuKey) & 0x8000) != 0;
    if (down && !g_keyWas) g_visible = !g_visible;
    g_keyWas = down;
    if (g_cfg.inputBlock) InputGuard::Tick(MenuOwnsInput());
    if (!g_visible) return g_origEndScene(dev);

    if (!g_imgui) InitImGui(dev);
    if (g_imgui) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        if (g_bbWidth && g_bbHeight) ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(g_bbWidth), static_cast<float>(g_bbHeight));
        ImGui::NewFrame();
        DrawMenu();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    return g_origEndScene(dev);
}

HRESULT APIENTRY PresentHook(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r)
{
    LARGE_INTEGER t0 = PerfNow();
    HRESULT hr = g_origPresent(dev, a, b, w, r);
    PerfD3D(1, PerfMsSince(t0));
    return hr;
}
HRESULT APIENTRY DIPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) { PerfDraw(pc); return g_origDIP(d, t, bv, mi, nv, si, pc); }
HRESULT APIENTRY DPHook(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) { PerfDraw(pc); return g_origDP(d, t, sv, pc); }

HRESULT APIENTRY ResetHook(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (g_imgui) ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_origReset(dev, pp);
    Log("device Reset %ux%u -> 0x%08X%s", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, static_cast<unsigned>(hr), FAILED(hr) ? " FAILED" : "");
    if (SUCCEEDED(hr) && pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
    if (g_imgui && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

void HookDevice(IDirect3DDevice9* dev)
{
    if (g_deviceHooked || !dev) return;
    void** vt = *reinterpret_cast<void***>(dev);
    if (MH_CreateHook(vt[42], &EndSceneHook, reinterpret_cast<void**>(&g_origEndScene)) != MH_OK ||
        MH_CreateHook(vt[16], &ResetHook, reinterpret_cast<void**>(&g_origReset)) != MH_OK ||
        MH_EnableHook(vt[42]) != MH_OK || MH_EnableHook(vt[16]) != MH_OK) {
        Log("[FAIL] menu: EndScene/Reset hook failed - no menu, LOD A binding only through the game's loader");
        return;
    }
    g_deviceHooked = true;
    Log("[OK] device %p hooked (EndScene/Reset), menu key 0x%02X", dev, g_cfg.menuKey);
    // measurement only (perf.cpp): Present = time spent waiting for the GPU / vsync, draw calls counted
    if (MH_CreateHook(vt[17], &PresentHook, reinterpret_cast<void**>(&g_origPresent)) == MH_OK) MH_EnableHook(vt[17]);
    if (MH_CreateHook(vt[82], &DIPHook, reinterpret_cast<void**>(&g_origDIP)) == MH_OK) MH_EnableHook(vt[82]);
    if (MH_CreateHook(vt[81], &DPHook, reinterpret_cast<void**>(&g_origDP)) == MH_OK) MH_EnableHook(vt[81]);
}

HRESULT APIENTRY CreateDeviceHook(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    HRESULT hr = g_origCreateDevice(self, adapter, type, focus, behaviour, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
        HookDevice(*out);
        SettingsAfterDevice();
    }
    return hr;
}

IDirect3D9* WINAPI D3DCreateHook(UINT sdk)
{
    IDirect3D9* d3d = g_origD3DCreate(sdk);
    if (d3d && !g_origCreateDevice) g_origCreateDevice = reinterpret_cast<CreateDevice_t>(SwapVtable(d3d, 16, reinterpret_cast<void*>(&CreateDeviceHook)));
    return d3d;
}
} // namespace

void UiInstall(HMODULE self)
{
    g_self = self;
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { Log("[FAIL] MinHook init"); return; }
    g_origD3DCreate = reinterpret_cast<Direct3DCreate9_t>(PatchImport("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&D3DCreateHook)));
    Log(g_origD3DCreate ? "[OK] Direct3DCreate9 import redirected" : "[FAIL] Direct3DCreate9 import not found - no menu");
    if (g_cfg.inputBlock) {
        g_origDI8Create = reinterpret_cast<DI8Create_t>(PatchImport("dinput8.dll", "DirectInput8Create", reinterpret_cast<void*>(&DI8CreateHook)));
        InputGuard::Install(self, g_cfg.inputGuardModules);
        Log("input: %s", InputGuard::Status());
    }
}
