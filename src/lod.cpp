// LOD A everywhere - the core of ForceLOD and the reason it exists for RTX Remix: one mesh per object,
// every frame, in every view. Remix hashes the geometry it is given; a LOD switch is a different mesh
// and therefore a different hash (replacements and lights vanish).
//
// World: ScenerySection_AddInstance 0x5D6570 picks the LOD at 0x5D6653..0x5D673A (docs T72 §3):
//   px <= 1 culled before us; then 0x1800 views (reflections) px < 32 -> cull, B or C; view 3 / split screen
//   B; main views px < 17 -> cull, A only if bound, tris/score and World Detail allow it, otherwise B.
// Our hook replaces the whole block:
//   - our own pixel limit with a hysteresis band (no flicker at the edge),
//   - always A; objects whose data has no A at all keep their only mesh (B, else C) - stable as well,
//   - A not bound yet -> bound ON THE SPOT, exactly like the game's loader does it, and drawn as A in the same
//     frame. Never B. Only if the A solid is not in memory at all (its pack not loaded) nothing can be drawn
//     for that frame - the streamer side (LOD A units always wanted, docs LODA_UNITS.md) makes sure that
//     does not happen; the menu counts it in red.
// Registers at 0x5D6653 (identical to the original): eax = px, ebx = cull info (view), edi = SceneryInfo,
// esi = SceneryInstance. 0x5D673A expects ebx = chosen eModel*; 0x5D69A1 = culled; 0x5D671A = the
// game's own reflection branch (needs eax = px).
//
// Cars: two engine debug overrides 0x802728 (body) / 0x80272C (tyres), -1 = automatic, read by every car
// renderer and view after its own choice (T72 §5) -> 0 = LOD A. Plus: cullable parts, pixel limit,
// low-detail limit, respawn blinking.
#include "core.h"
#include <cstring>

namespace {
constexpr DWORD kLodSite    = 0x005D6653;
constexpr DWORD kLodResume  = 0x005D673A;
constexpr DWORD kLodCull    = 0x005D69A1;
constexpr DWORD kLodReflect = 0x005D671A;
const BYTE kLodOrig[6] = { 0x8B, 0x5B, 0x44, 0xF6, 0xC7, 0x18 };   // mov ebx,[ebx+44h] ; test bh,18h

// SceneryInfo (stride 0x44): +0x20 LOD hashes A/B/C, +0x30 eModel* A/B/C, +0x3C radius. eModel +0x0C eSolid*.
// SceneryInstance (stride 0x40): +0x18 info index, +0x1A flags.
// Cull info: +0x44 flags (0x1800 reflection / env views), +0x4C list cursor, +0x50 list end.

// counters, running on the render thread; LodFrame turns them into per-frame values
LodCounters g_run = {}, g_last = {};
bool  g_hooked = false;
int   g_minPx = 4, g_hyst = 2, g_reflA = 1, g_extraPx = 17;
DWORD g_result = 0;
DWORD g_ptrResume = kLodResume, g_ptrCull = kLodCull, g_ptrReflect = kLodReflect;

// Hysteresis: direct-mapped by instance pointer. A collision only loses the band for that instance.
struct PxState { const void* inst; BYTE visible; };
PxState g_px[2][8192];   // [0] player views, [1] reflection / env views - their limits differ

bool PixelVisible(const void* inst, int px, int limit, int table)
{
    PxState& e = g_px[table][(reinterpret_cast<DWORD>(inst) >> 6) & 8191];
    if (px >= limit + g_hyst) { e.inst = inst; e.visible = 1; return true; }
    if (px < limit)           { e.inst = inst; e.visible = 0; return false; }
    if (e.inst == inst) return e.visible != 0;   // inside the band: keep the last decision
    e.inst = inst; e.visible = 1; return true;
}

// Binding exactly like the game's loader (eModel::Init 0x493600 -> eFindSolid 0x48DF90 -> SetSolid 0x48D780),
// in the culler (main thread; SetSolid only moves the model from the unbound list 0x82DBC0 to the solid's list -
// the culler reads model+0xC and nothing else of those lists) and never while the streamer holds its lock [0x864F60].
typedef void* (__cdecl* FindSolid_t)(unsigned hash, int flags);
typedef void  (__thiscall* SetSolid_t)(void* model, void* solid);
const FindSolid_t eFindSolid     = reinterpret_cast<FindSolid_t>(0x0048DF90);
const SetSolid_t  eModelSetSolid = reinterpret_cast<SetSolid_t>(0x0048D780);

// Negative cache: a LOD A hash whose solid is not in memory is looked up again only every 30 frames - eFindSolid walks
// the pack headers on a bucket collision, and the culler would otherwise repeat that per instance, view and frame.
struct MissEntry { unsigned hash; unsigned frame; };
MissEntry g_miss[4096];
unsigned g_frame = 0;

bool BindOne(const BYTE* info)
{
    BYTE* model = *reinterpret_cast<BYTE* const*>(info + 0x30);
    if (!model) return false;
    if (*reinterpret_cast<void* const*>(model + 0x0C)) return true;
    unsigned hash = *reinterpret_cast<const unsigned*>(info + 0x20);
    if (!hash || hash == 0xBE43EDBB || hash == 0x72923C81) return false;        // the loader's "no model" hashes
    if (*reinterpret_cast<const int*>(0x00864F60) != 0) return false;            // chunks being relocated (pointer delta): the loader does not bind either
    MissEntry& m = g_miss[(hash ^ (hash >> 12)) & 4095];
    if (m.hash == hash && g_frame - m.frame < 30) { ++g_run.bindMissing; return false; }
    __try {
        void* solid = eFindSolid(hash, 0);
        if (!solid) { m.hash = hash; m.frame = g_frame; ++g_run.bindMissing; return false; }   // pack not in memory
        eModelSetSolid(model, solid);
        ++g_run.bound;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return *reinterpret_cast<void* const*>(model + 0x0C) != nullptr;
}

// Returns the eModel* to draw, 0 = cull, 1 = the game's own reflection branch.
DWORD __cdecl LodDecide(const BYTE* view, const BYTE* info, const BYTE* inst, int px)
{
    DWORD flags = *reinterpret_cast<const DWORD*>(view + 0x44);
    bool refl = (flags & 0x1800) != 0;
    if (refl && !g_reflA) { ++g_run.refBC; return 1; }
    int limit = refl ? (g_minPx > 32 ? g_minPx : 32) : g_minPx;
    if (g_fullMapExtraWalk && g_extraPx > limit) limit = g_extraPx;   // far extra sections: own limit (performance only)
    const bool capture = !refl && (FullMapCapture() || g_guardWalk);   // capture / guard radius: no pixel limit at all
    if (!capture && !PixelVisible(inst, px, limit, refl ? 1 : 0)) { ++g_run.pixelCulled; return 0; }
    if (*reinterpret_cast<const DWORD*>(view + 0x4C) >= *reinterpret_cast<const DWORD*>(view + 0x50)) ++g_run.listFull;   // the game drops it right after us
    const BYTE* a = *reinterpret_cast<const BYTE* const*>(info + 0x30);
    if (a) {
        if (*reinterpret_cast<const void* const*>(a + 0x0C)) { ++g_run.drawnA; return reinterpret_cast<DWORD>(a); }
        if (BindOne(info)) { ++g_run.drawnA; return reinterpret_cast<DWORD>(a); }
        ++g_run.unboundCulled;   // A solid not in memory: nothing to draw (never B); tried again next frame
        return 0;
    }
    // no LOD A in the data at all: its only mesh, always the same one
    const BYTE* b = *reinterpret_cast<const BYTE* const*>(info + 0x34);
    const BYTE* m = b ? b : *reinterpret_cast<const BYTE* const*>(info + 0x38);
    if (m) ++g_run.drawnNoA;
    return reinterpret_cast<DWORD>(m);
}

__declspec(naked) void LodHook()
{
    __asm {
        pushad
        push eax                    // px
        push esi                    // instance
        push edi                    // info
        push ebx                    // cull info
        call LodDecide
        add  esp, 16
        mov  g_result, eax
        popad
        mov  ebx, g_result
        cmp  ebx, 1
        je   reflect
        test ebx, ebx
        jz   cull
        jmp  dword ptr [g_ptrResume]
    reflect:
        jmp  dword ptr [g_ptrReflect]
    cull:
        jmp  dword ptr [g_ptrCull]
    }
}

// --- LOD A units always wanted (docs LODA_UNITS.md) -------------------------------------------------
// 85 % of the placed objects in L4RA keep their LOD A solid in the shared X units (mostly X00, area packs X01..X25),
// their textures in the matching Y units. Vanilla wants X00/Y00 always and one area pack per zone of the CAR - enough
// for vanilla, not for anything drawn beyond the car's zone list (full map, guard band). So after the game's own
// BuildWantedList 0x5E5110 (only caller: SwitchZones, call at 0x5E7BEC, ecx = TheTrackStreamer) every X/Y unit below
// 50 of the loaded region is added through AddSectionsToLoad 0x5DACB0 (thiscall, int16 list, count, slot; ret 0Ch),
// for both slots like the game's fixed list. Read from the unit table at run time (mod tracks too). Cost: L4RA +3.3 MB,
// L4RD +6.1 MB, the other regions 0. Not in split screen (the game draws B there and remaps X/Y to 50+).
typedef void (__thiscall* BuildWanted_t)(void* ts);
typedef int  (__thiscall* AddSections_t)(void* ts, short* units, int count, int slot);
const BuildWanted_t kBuildWanted = reinterpret_cast<BuildWanted_t>(0x005E5110);
const AddSections_t kAddSections = reinterpret_cast<AddSections_t>(0x005DACB0);
int g_lodaUnits = 0;

void __fastcall BuildWantedHook(void* ts, void*)
{
    kBuildWanted(ts);
    if (*reinterpret_cast<const int*>(0x0089E7C0) == 2) return;           // two players
    const bool all = FullMapWantAll();                                    // full map: every unit except the split-screen X/Y 50+
    if (!all && !g_cfg.lodaUnits) return;
    static short list[1024], copy[1024]; int n = 0;
    const BYTE* u = *reinterpret_cast<const BYTE* const*>(ts);
    int count = reinterpret_cast<const int*>(ts)[1];
    for (int i = 0; u && i < count && n < 1024; ++i, u += 0x50) {
        short num = *reinterpret_cast<const short*>(u + 8);
        const int letter = num / 100;                                      // 1..26 = A..Z; anything else (e.g. unit 0) is no section
        if (letter < 1 || letter > 26) continue;
        const bool xy = letter == 24 || letter == 25;
        if (xy ? num % 100 < 50 : all) list[n++] = num;
    }
    if (!n) return;
    memcpy(copy, list, n * sizeof(short));                                // the game may rewrite the list it is given
    kAddSections(ts, list, n, 0);
    kAddSections(ts, copy, n, 1);
    if (n != g_lodaUnits) { g_lodaUnits = n; Log(all ? "full map: %d units wanted (whole region)" : "LOD A units: %d X/Y units kept wanted", n); }
}

// --- car and part patches ----------------------------------------------------------------------
int g_hBooBoo = -1;
int g_hCarLod = -1, g_hTyreLod = -1, g_hCullable = -1, g_hCarPx[3] = { -1, -1, -1 }, g_hLowDetail = -1, g_hBlink = -1;
const DWORD kCarPxSites[3] = { 0x00620CB9, 0x00616343, 0x006156E1 };   // imm8 of `cmp edi/esi, 4`: Render, RenderFast, RenderFlaresOnCar

void SetIntPatch(int h, int value, int vanilla)
{
    if (h < 0) return;
    if (value == vanilla) { PatchSet(h, false); return; }
    PatchSetRepl(h, &value);
    PatchSet(h, true);
}
} // namespace

void LodInstall()
{
    g_minPx = g_cfg.worldMinPx; g_hyst = g_cfg.worldPxHyst; g_reflA = g_cfg.reflectionLodA; g_extraPx = g_cfg.extraMinPx;
    int h = PatchAddJmp("world LOD A", "lod", kLodSite, kLodOrig, 6, reinterpret_cast<const void*>(&LodHook),
                        "LOD choice of every scenery instance: always A, unbound A culled, own pixel limit");
    g_hooked = PatchSet(h, true) && PatchAt(h)->state == PS_On;
    if (!g_hooked) Log("[FAIL] world LOD A hook not installed - the world keeps the game's LOD switching");
    {
        const BYTE orig[5] = { 0xE8, 0x1F, 0xD5, 0xFF, 0xFF };   // call BuildWantedList
        BYTE repl[5] = { 0xE8 };
        *reinterpret_cast<DWORD*>(repl + 1) = reinterpret_cast<DWORD>(&BuildWantedHook) - (0x005E7BEC + 5);
        int hw = PatchAdd("LOD A units always wanted", "lod", 0x005E7BEC, orig, repl, 5, "X/Y units < 50 (shared LOD A solids + textures) never unloaded");
        if (g_cfg.lodaUnits || g_cfg.fullMap) PatchSet(hw, true);
    }

    // Precull BooBoo map (TRACKS\PrecullerBooBooScript.hoo, 0x8832C0, 128x128 bits of 32 m): in a listed camera cell
    // BuildCullLists switches precull off for every view (0x5E9B54 test -> 0x5E9B5C xor bl,bl) - the whole zone list is then
    // drawn without occlusion data. jz -> jmp at 0x5E9B5A: precull always on.
    const BYTE booOrig[2] = { 0x74, 0x02 }, booNew[2] = { 0xEB, 0x02 };
    g_hBooBoo = PatchAdd("precull BooBoo map off", "lod", 0x005E9B5A, booOrig, booNew, 2, "BuildCullLists: precull never switched off by the BooBoo cells");
    const int minus1 = -1;
    g_hCarLod  = PatchAdd("car body LOD override", "lod", 0x00802728, &minus1, &g_cfg.carLod, 4, "engine debug override, -1 = automatic, 0 = A");
    g_hTyreLod = PatchAdd("car tyre LOD override", "lod", 0x0080272C, &minus1, &g_cfg.carTyreLod, 4, "engine debug override, -1 = automatic, 0 = A");
    const BYTE c0 = 0x00, c1 = 0x01;
    g_hCullable = PatchAdd("cullable car parts", "lod", 0x0060C5A9, &c0, &c1, 1, "CarPartCuller: underbody/brakes/sides never hidden by view angle");
    const char* pxNames[3] = { "car min pixels (Render)", "car min pixels (RenderFast)", "car min pixels (flares)" };
    const BYTE four = 4; BYTE px = static_cast<BYTE>(g_cfg.carMinPx);
    for (int i = 0; i < 3; ++i) g_hCarPx[i] = PatchAdd(pxNames[i], "lod", kCarPxSites[i], &four, &px, 1, "cars below N px are not drawn (vanilla 4)");
    const float lowOrig = 18.0f, lowNew = static_cast<float>(g_cfg.carLowDetailPx);
    g_hLowDetail = PatchAdd("car low-detail limit", "lod", 0x007A66FC, &lowOrig, &lowNew, 4, "below N px no wheels/brakes/attachments (vanilla 18)");
    const BYTE blinkOrig[2] = { 0x74, 0x1E }, blinkNew[2] = { 0xEB, 0x1E };
    g_hBlink = PatchAdd("car respawn blink off", "lod", 0x0060D544, blinkOrig, blinkNew, 2, "ShouldCarBeRendered: blinking car hidden every 2nd frame");
    LodApplyLive();
}

void LodApplyLive()
{
    g_minPx = g_cfg.worldMinPx; g_hyst = g_cfg.worldPxHyst; g_reflA = g_cfg.reflectionLodA; g_extraPx = g_cfg.extraMinPx;
    SetIntPatch(g_hCarLod, g_cfg.carLod, -1);
    SetIntPatch(g_hTyreLod, g_cfg.carTyreLod, -1);
    PatchSet(g_hCullable, g_cfg.cullableParts != 0);
    BYTE px = static_cast<BYTE>(g_cfg.carMinPx);
    for (int i = 0; i < 3; ++i) { PatchSetRepl(g_hCarPx[i], &px); PatchSet(g_hCarPx[i], px != 4); }
    float low = static_cast<float>(g_cfg.carLowDetailPx);
    PatchSetRepl(g_hLowDetail, &low); PatchSet(g_hLowDetail, g_cfg.carLowDetailPx != 18);
    PatchSet(g_hBlink, g_cfg.carBlinkOff != 0);
    PatchSet(g_hBooBoo, g_cfg.boobooOff != 0);
}

void LodFrame()
{
    ++g_frame;
    // every 10 s while driving: the counters of the last frame, so a test run can be read from the log alone
    static DWORD lastLog = 0;
    DWORD now = GetTickCount();
    if (*reinterpret_cast<const int*>(0x008654A4) == 6 && now - lastLog >= 10000) {
        lastLog = now;
        // free address space: a non-LAA SPEED2.EXE has 2 GB - texture creation fails silently when it runs out (black textures)
        DWORD freeMB = 0, largestMB = 0; MEMORY_BASIC_INFORMATION mbi;
        for (DWORD p = 0x10000; p < 0xFFFE0000u && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)); ) {
            DWORD sz = static_cast<DWORD>(mbi.RegionSize);
            if (mbi.State == MEM_FREE) { freeMB += sz >> 20; if ((sz >> 20) > largestMB) largestMB = sz >> 20; }
            if (p + sz <= p) break;
            p += sz;
        }
        Log("[lod] frame: A %u, no-A-in-data %u, refl B/C %u, A missing->culled %u, bound now %u, bind failed %u, px culled %u, list full %u | car override %d/%d | address space free %u MB, largest %u MB",
            g_run.drawnA, g_run.drawnNoA, g_run.refBC, g_run.unboundCulled, g_run.bound, g_run.bindMissing, g_run.pixelCulled, g_run.listFull,
            *reinterpret_cast<const int*>(0x00802728), *reinterpret_cast<const int*>(0x0080272C), freeMB, largestMB);
    }
    g_last = g_run;
    g_run = LodCounters{};
}

// batch.cpp (loading screen): the same model LodDecide draws - A (bound on the spot), else the only mesh of the data
const BYTE* LodModelForBatch(const BYTE* info)
{
    const BYTE* a = *reinterpret_cast<const BYTE* const*>(info + 0x30);
    if (a) return (*reinterpret_cast<const void* const*>(a + 0x0C) || BindOne(info)) ? a : nullptr;
    const BYTE* b = *reinterpret_cast<const BYTE* const*>(info + 0x34);
    return b ? b : *reinterpret_cast<const BYTE* const*>(info + 0x38);
}

LodCounters LodLastFrame() { return g_last; }
bool LodHooked() { return g_hooked; }
