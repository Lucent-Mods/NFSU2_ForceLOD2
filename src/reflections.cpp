// Reflection render-target sizes (Vanilla profile; from the old ForceLOD, which moved them to the ShaderLoader on
// 2026-09-28 - same sites, same values). Startup only: eCreateRenderTargets runs after the device, long after us.
//   road reflection texture + depth surface 320x240: push imm32 at 0x5BA08A/0x5BA08F (texture), 0x5BA0CC/0x5BA0D1
//   (depth), view size 0x5C2366/0x5C236D (mov [edi+14/18], imm32); car env-map cube face int 0x7FEE6C (128);
//   rear-view mirror ints 0x7FEE80/0x7FEE84 (256).
// HD Reflections patches the same places: when its .asi is in the folder, or any value is no longer the original,
// the whole feature stays off - never two plugins on the same bytes. Remix profile: nothing (the game's reflection
// views are switched off for Remix anyway).
#include "core.h"
#include <cstdio>

namespace {
struct Site { DWORD at; DWORD orig; };
const Site kRoad[6] = { {0x005BA08A, 0xF0}, {0x005BA08F, 0x140}, {0x005BA0CC, 0xF0}, {0x005BA0D1, 0x140}, {0x005C2366, 0x140}, {0x005C236D, 0xF0} };
const Site kEnv       = { 0x007FEE6C, 128 };
const Site kMirror[2] = { {0x007FEE80, 256}, {0x007FEE84, 256} };

bool Original(const Site* s, int n)
{
    for (int i = 0; i < n; ++i) if (*reinterpret_cast<const DWORD*>(s[i].at) != s[i].orig) return false;
    return true;
}
const char* g_status = "vanilla sizes";
} // namespace

void ReflectionsInstall()
{
    const int road = g_cfg.roadReflScale, env = g_cfg.carEnvMapSize, mirror = g_cfg.mirrorSize;
    if (g_cfg.profile != PROFILE_VANILLA) { g_status = "Remix profile: game reflections off, sizes vanilla"; return; }
    if (road == 1 && !env && !mirror) { Log("reflections: render-target sizes vanilla (nothing set)"); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\NFSU2HDReflections.asi", g_baseDir);
    if (GetModuleHandleA("NFSU2HDReflections.asi") || GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
        g_status = "HD Reflections installed - it owns the sizes, ours are off";
        Log("[SKIP] reflections: HD Reflections installed - it sets the reflection sizes");
        return;
    }
    if (!Original(kRoad, 6) || !Original(&kEnv, 1) || !Original(kMirror, 2)) {
        g_status = "another plugin already changed the sizes - ours are off";
        Log("[SKIP] reflections: render-target sizes already changed by another plugin");
        return;
    }
    char id[48];
    if (road > 1) for (const Site& s : kRoad) {
        const DWORD v = s.orig * road;
        snprintf(id, sizeof(id), "road reflection size %06X", s.at);
        PatchSet(PatchAdd(id, "refl", s.at, &s.orig, &v, 4, "320x240 x scale (texture, depth, view)"), true);
    }
    if (env) PatchSet(PatchAdd("car env-map cube size", "refl", kEnv.at, &kEnv.orig, &env, 4, "cube face 128 -> N"), true);
    if (mirror) for (const Site& s : kMirror) {
        snprintf(id, sizeof(id), "mirror size %06X", s.at);
        PatchSet(PatchAdd(id, "refl", s.at, &s.orig, &mirror, 4, "rear-view mirror 256 -> N"), true);
    }
    static char status[96];
    snprintf(status, sizeof(status), "road %dx%d, car env-map %d, mirror %d", 320 * road, 240 * road, env ? env : 128, mirror ? mirror : 256);
    g_status = status;
    Log("[OK] reflections: %s", status);
}

const char* ReflectionsStatus() { return g_status; }
