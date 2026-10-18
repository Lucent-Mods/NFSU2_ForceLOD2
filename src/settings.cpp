// Hidden engine settings and the views a Remix setup does not want.
//
// The values are registry-backed globals the renderer reads every frame (0x870CB8..0x870D10). They are
// written live and re-asserted at the entry of RenderFrame 0x5D2850 (`mov al,[0x8709D1]`, once per frame) - so
// the options menu never wins over a forced value. Note: the options screen saves what it shows to the registry
// (0x5BEEA0), so forced values stay in the registry after the plugin is removed.
// Render targets: start-up order 0x5D24F0 = Direct3DCreate9, registry 0x5D2522 (writes 0x870CB8 / 0x870CD4),
// device 0x5D2749, eCreateRenderTargets 0x5D27C1. SettingsAfterDevice runs right after CreateDevice (ui.cpp), i.e.
// after the registry and before the targets: forcing the car env map / road reflection option to 0 there is
// exactly vanilla "option off" - the cube and the road target are never created, the views never run.
#include "core.h"

namespace {
constexpr DWORD kApplySite = 0x005D2850;
const BYTE kApplyOrig[5] = { 0xA0, 0xD1, 0x09, 0x87, 0x00 };
DWORD g_ptrResume = 0x005D2855;

void __cdecl ApplyHidden()
{
    for (int i = 0; i < HS_Count; ++i) {
        int want = g_cfg.hidden[i];
        if (want < 0) continue;
        int* global = reinterpret_cast<int*>(kHidden[i].addr);
        if (*global != want) *global = want;
    }
}

__declspec(naked) void ApplyHook()
{
    __asm {
        pushad
        pushfd
        call ApplyHidden
        popfd
        popad
        mov  al, byte ptr ds:[0x008709D1]
        jmp  dword ptr [g_ptrResume]
    }
}
} // namespace

void SettingsInstall()
{
    int h = PatchAddJmp("hidden settings re-assert", "settings", kApplySite, kApplyOrig, 5, reinterpret_cast<const void*>(&ApplyHook),
                        "RenderFrame entry: forced values re-applied every frame");
    if (!PatchSet(h, true)) Log("[INFO] hidden settings: hook site taken, values are only written live");
    ApplyHidden();
}

void SettingsApplyLive() { ApplyHidden(); }
void SettingsAfterDevice() { ApplyHidden(); }
