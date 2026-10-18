// Full map: the whole region in memory and drawn as real geometry (LOD A through lod.cpp) instead of the coarse
// stand-ins the game shows for everything outside the camera zone's list. Rule and data: docs T91 / T92
// (ShaderLoader docs/re/rebuild_20261017), tables generated offline (fullmap_tables.h, T92 §9).
//
// 1. Load everything: after BuildWantedList every unit of the region is wanted (lod.cpp BuildWantedHook asks
//    FullMapWantAll), except the split-screen set X/Y 50+. Needs the streamer limits of pools.cpp (heap nodes,
//    wanted/deferred lists 1024) and the streaming pool at 200 %.
// 2. Activate everything in the loading screen: the call 0x57FA11 (loader step 7) also runs
//    TrackStreamer_BlockUntilLoaded 0x5E79D0 - vanilla activates one drivable unit per frame after the start.
// 3. Replay snapshot buffer +4 KB (0x57908F / 0x5790AB): one byte per wanted drivable unit per snapshot.
// 4. DrawVisibleSections 0x5E99C0 replaced, per view (cull info):
//      - exactly what vanilla draws: GetVisibleSectionList 0x5E96B0 (26xx + the camera zone's list), precull as vanilla;
//      - in player views (flags 0x6000) every other loaded real section as "extra", precull off (the 32 m precull cells
//        wrap every 1024 m - their bits mean nothing for other zones, T91 §6), except
//        panoramas, sections never loaded, and the zone's blocked list (B1 inside 8 m of a shown panorama, B2 duplicate
//        instances, B3 clear overlap - T92 §2). Nothing vanilla shows is ever dropped.
//    Regions without a table (mod maps) draw exactly vanilla.
#include "core.h"
#include "fullmap_tables.h"
#include "fullmap_replace.h"
#include <cstring>
#include <algorithm>

extern bool g_fullMapExtraWalk;

namespace {
struct RegionRules {
    const char* name; const int16_t* pan; int panN; const int16_t* never; int neverN;
    const int16_t* blocked; const int32_t (*zone)[3]; int zoneN;
    // replace mode (tools/fullmap_replace_emit.py): city panoramas covered by real geometry are hidden and only the
    // blocks of the kept panoramas (terrain, ocean, landmarks without counterpart) and B2/B3 remain
    const int16_t* rdrop; int rdropN; const int16_t* rblocked; const int32_t (*rzone)[3]; int rzoneN;
};
#define RULES(tag, NAME) { NAME, k_##tag##_pan, sizeof(k_##tag##_pan) / 2, k_##tag##_never, sizeof(k_##tag##_never) / 2, \
                           k_##tag##_blocked, k_##tag##_zone, sizeof(k_##tag##_zone) / 12, \
                           k_##tag##_rdrop, sizeof(k_##tag##_rdrop) / 2, k_##tag##_rblocked, k_##tag##_rzone, sizeof(k_##tag##_rzone) / 12 }
const RegionRules kRegions[] = {
    RULES(l4ra, "L4RA"), RULES(l4rb, "L4RB"), RULES(l4rc, "L4RC"), RULES(l4rd, "L4RD"),
    RULES(l4rf, "L4RF"), RULES(l4rg, "L4RG"), RULES(l4rh, "L4RH"), RULES(l4rr, "L4RR"),
};

constexpr int kMaxSection = 0xA8C;              // slot table 0x883EC0 has 2700 entries
const RegionRules* g_rules = nullptr;
char  g_regionName[16] = "";
BYTE  g_isPan[kMaxSection], g_isNever[kMaxSection], g_isDrop[kMaxSection];
bool  g_replace = false;
bool  g_enabled = false;
FullMapStats g_stats = {};

// per-zone blocked bitset, cached for the last zone (views of one frame mostly share it)
int   g_blockedZone = -1;
BYTE  g_blocked[kMaxSection];
BYTE  g_listed[kMaxSection];
BYTE  g_walked[kMaxSection];   // per view: each section walked at most once, whatever the loaded list holds

typedef int   (__stdcall*  GetVisible_t)(short* out, int max, BYTE* ci);
typedef void* (__thiscall* FindDrivable_t)(void* mgr, const float* pos);
typedef void  (__thiscall* Walk_t)(void* header, BYTE* ci);
typedef void  (__thiscall* AddInst_t)(void* header, int idx, BYTE* ci, int state);
const AddInst_t kAddInst = reinterpret_cast<AddInst_t>(0x005D6570);
const GetVisible_t   kGetVisible   = reinterpret_cast<GetVisible_t>(0x005E96B0);
const FindDrivable_t kFindDrivable = reinterpret_cast<FindDrivable_t>(0x005E00D0);
const Walk_t         kWalk         = reinterpret_cast<Walk_t>(0x005D69B0);

BYTE* SectionHeader(int s)
{
    if (s < 0 || s >= kMaxSection) return nullptr;
    BYTE* slot = reinterpret_cast<BYTE* const*>(0x00883EC0)[s];
    if (!slot) return nullptr;
    BYTE* hdr = *reinterpret_cast<BYTE* const*>(slot + 4);
    if (!hdr || !*reinterpret_cast<void* const*>(hdr + 0x28)) return nullptr;
    return hdr;
}

// last drivable zone the camera was in (region data, reset with the region)
const BYTE* g_lastZone = nullptr;

// GetVisibleSectionList 0x5E96B0 with a given zone: 26xx + the zone's list (+0x10 count, +0x14 int16), loaded ones only
int ZoneList(const BYTE* zone, short* out, int max)
{
    int n = 0;
    for (int s = 2600; s < 2700 && n < max; ++s) {
        const BYTE* slot = reinterpret_cast<const BYTE* const*>(0x00883EC0)[s];
        if (slot && *reinterpret_cast<void* const*>(slot + 4)) out[n++] = static_cast<short>(s);
    }
    const int cnt = *reinterpret_cast<const int*>(zone + 0x10);
    const short* l = reinterpret_cast<const short*>(zone + 0x14);
    for (int i = 0; i < cnt && n < max; ++i) {
        const int s = l[i];
        if (s < 0 || s >= kMaxSection) continue;
        const BYTE* slot = reinterpret_cast<const BYTE* const*>(0x00883EC0)[s];
        if (slot && *reinterpret_cast<void* const*>(slot + 4)) out[n++] = static_cast<short>(s);
    }
    return n;
}

void PickRegion()
{
    const BYTE* info = *reinterpret_cast<const BYTE* const*>(0x00883DAC);   // TrackInfo::LoadedTrackInfo
    char name[16] = "";
    if (info) { memcpy(name, info + 0x40, 15); name[15] = 0; }
    if (g_rules && strcmp(name, g_regionName) == 0) return;
    memcpy(g_regionName, name, sizeof(g_regionName));
    g_rules = nullptr; g_blockedZone = -1; g_lastZone = nullptr;
    for (const RegionRules& r : kRegions) if (_stricmp(r.name, name) == 0) { g_rules = &r; break; }
    memset(g_isPan, 0, sizeof(g_isPan)); memset(g_isNever, 0, sizeof(g_isNever)); memset(g_isDrop, 0, sizeof(g_isDrop));
    if (g_rules) {
        for (int i = 0; i < g_rules->panN; ++i)   { int s = g_rules->pan[i];   if (s >= 0 && s < kMaxSection) g_isPan[s] = 1; }
        for (int i = 0; i < g_rules->neverN; ++i) { int s = g_rules->never[i]; if (s >= 0 && s < kMaxSection) g_isNever[s] = 1; }
        if (g_replace) for (int i = 0; i < g_rules->rdropN; ++i) { int s = g_rules->rdrop[i]; if (s >= 0 && s < kMaxSection) g_isDrop[s] = 1; }
    }
    Log("full map: region '%s' -> %s", name, g_rules ? "rules loaded (extras drawn)" : "no rule table: vanilla drawing");
}

void BuildBlocked(int zone)
{
    if (zone == g_blockedZone) return;
    g_blockedZone = zone;
    memset(g_blocked, 0, sizeof(g_blocked));
    const int32_t (*zt)[3] = g_replace ? g_rules->rzone : g_rules->zone;
    const int zn = g_replace ? g_rules->rzoneN : g_rules->zoneN;
    const int16_t* bl = g_replace ? g_rules->rblocked : g_rules->blocked;
    for (int i = 0; i < zn; ++i) {
        if (zt[i][0] != zone) continue;
        const int16_t* b = bl + zt[i][1];
        for (int k = 0; k < zt[i][2]; ++k) if (b[k] >= 0 && b[k] < kMaxSection) g_blocked[b[k]] = 1;
        return;
    }
}

// Profiles (2026-10-17, user rule):
//   Vanilla: full map + extras + batching (everything below).
//   Remix, capture off: EXACTLY vanilla - the game's streaming and section list, only LOD A (lod.cpp). No full map.
//   Remix, capture on: whole region loaded, EVERY object of every loaded section drawn - no occlusion table, no
//     frustum, no pixel limit - except the panoramas. Never batching.
bool WantAll() { return g_rules && !(g_cfg.profile == PROFILE_REMIX && !g_cfg.captureAll); }

// capture switched live: pixel-size cull patch and a streamer refresh (BuildWantedList with the new rule)
int  g_hCapturePx = -1;
bool g_captureOn = false;
// Guard radius (Remix profile without capture, plan phase 3): the listed sections near the camera are walked instance
// by instance instead of through the cull tree - inside GuardRadius state 2 (no frustum test), precull off and no pixel
// limit (the px <= 1 cull at 0x5D6646 is off too: behind the camera px is 0); inside PrecullOffRadius precull off,
// frustum and pixel limit as usual. Farther sections: the game's own walk.
bool GuardActive() { return g_enabled && g_cfg.profile == PROFILE_REMIX && !g_cfg.captureAll && g_cfg.guardRadius > 0; }
bool g_guardOn = false;

float BoxDist2(const float* mn, const float* mx, const float* p)
{
    float d2 = 0;
    for (int k = 0; k < 3; ++k) { const float d = p[k] < mn[k] ? mn[k] - p[k] : (p[k] > mx[k] ? p[k] - mx[k] : 0.f); d2 += d * d; }
    return d2;
}

int g_guardInst = 0;
void GuardWalk(BYTE* h, BYTE* ci)
{
    const float* cam = reinterpret_cast<const float*>(ci + 0x60);
    const float g2 = static_cast<float>(g_cfg.guardRadius) * g_cfg.guardRadius;
    const float p2 = static_cast<float>(g_cfg.precullOffRadius) * g_cfg.precullOffRadius;
    int& precull = *reinterpret_cast<int*>(ci + 0x84);
    const int keep = precull;
    const BYTE* insts = *reinterpret_cast<const BYTE* const*>(h + 0x20);
    const int n = *reinterpret_cast<const int*>(h + 0x24);
    for (int i = 0; i < n; ++i) {
        const float* b = reinterpret_cast<const float*>(insts + i * 0x40);   // world box min +0x00, max +0x0C
        const float d2 = BoxDist2(b, b + 3, cam);
        if (d2 <= g2) { precull = -1; g_guardWalk = true; kAddInst(h, i, ci, 2); g_guardWalk = false; ++g_guardInst; }
        else { precull = d2 <= p2 ? -1 : keep; kAddInst(h, i, ci, 1); }
    }
    precull = keep;
}

void SyncCapture()
{
    const bool on = FullMapCapture();
    const bool guard = GuardActive();
    if (guard != g_guardOn) {
        g_guardOn = guard;
        if (g_hCapturePx >= 0 && !on) PatchSet(g_hCapturePx, guard);
        Log("guard radius %s", guard ? "ON (Remix): nothing near the camera is culled" : "OFF");
    }
    if (on == g_captureOn) return;
    g_captureOn = on;
    if (g_hCapturePx >= 0) PatchSet(g_hCapturePx, on || guard);
    *reinterpret_cast<BYTE*>(0x0088ED88) = 1;     // TrackStreamer+0x288: DetermineZones reports a change -> SwitchZones -> BuildWantedList
    Log("capture %s", on ? "ON: whole region streamed in, every object except panoramas drawn" : "OFF: Remix draws exactly vanilla");
}

// World flares (lamps, neon - flare packs at slot+8): RenderWorldFlares 0x491860 walks only the camera zone's list, so
// the far sections we draw had no lights. With our drawing on (Vanilla extras / Remix capture) it walks exactly the
// sections drawn in the last player view instead; otherwise (Remix without capture) the game's own function.
short g_flareSecs[kMaxSection];
int   g_flareN = 0, g_flareBuild = 0;
bool  g_flareValid = false;
void FlareAdd(int s) { if (g_flareBuild < kMaxSection) g_flareSecs[g_flareBuild++] = static_cast<short>(s); }

typedef int (__cdecl* WorldFlares_t)(BYTE* view, int mode);
typedef int (__cdecl* LightFlare_t)(BYTE* view, BYTE* flare, const float* m, DWORD one, int a5, int mode, float elev, int a8);
typedef int (__thiscall* VisState_t)(const void* view, const float* mn, const float* mx, void* m);
WorldFlares_t g_origWorldFlares = nullptr;
const LightFlare_t kLightFlare = reinterpret_cast<LightFlare_t>(0x00490920);
const VisState_t   kVisState   = reinterpret_cast<VisState_t>(0x0048AC60);
typedef void* (__cdecl* GetView_t)(int i);
const GetView_t    kGetView    = reinterpret_cast<GetView_t>(0x0048B1E0);
int __cdecl WorldFlaresHook(BYTE* view, int mode)
{
    if (!g_flareValid || !*reinterpret_cast<const int*>(0x007F3D40) || *reinterpret_cast<const int*>(0x008654A4) != 6)
        return g_origWorldFlares(view, mode);
    const float* identity = reinterpret_cast<const float*>(0x00872E90);
    for (int i = 0; i < g_flareN; ++i) {
        const BYTE* slot = reinterpret_cast<BYTE* const*>(0x00883EC0)[g_flareSecs[i]];
        BYTE* pack = slot ? *reinterpret_cast<BYTE* const*>(slot + 8) : nullptr;
        if (!pack) continue;
        if (!kVisState(view, reinterpret_cast<const float*>(pack + 48), reinterpret_cast<const float*>(pack + 64), nullptr)) continue;
        BYTE* head = pack + 88;
        for (BYTE* f = *reinterpret_cast<BYTE**>(head); f && f != head; f = *reinterpret_cast<BYTE**>(f))
            kLightFlare(view, f, identity, 0x3F800000, 0, mode, 0.0f, 0);
    }
    return 0;
}

// capture: every instance of every loaded section except panoramas, state 2 (no frustum test), precull off
void DrawCapture(BYTE* ci)
{
    int& precull = *reinterpret_cast<int*>(ci + 0x84);
    const int keep = precull; precull = -1;
    g_fullMapExtraWalk = true;
    memset(g_walked, 0, sizeof(g_walked));
    int secs = 0, insts = 0, nodes = 0;
    const BYTE* head = reinterpret_cast<const BYTE*>(0x0088EAD4);
    for (const BYTE* node = *reinterpret_cast<const BYTE* const*>(head); node && node != head && nodes < 8192; node = *reinterpret_cast<const BYTE* const*>(node), ++nodes) {
        const int s = *reinterpret_cast<const int*>(node + 0x0C);
        if (s < 0 || s >= kMaxSection || g_walked[s]) continue;
        g_walked[s] = 1;
        if (g_isPan[s] || g_isNever[s]) continue;
        BYTE* h = SectionHeader(s);
        if (!h) continue;
        const int n = *reinterpret_cast<const int*>(h + 0x24);
        for (int i = 0; i < n; ++i) kAddInst(h, i, ci, 2);
        FlareAdd(s);
        ++secs; insts += n;
    }
    g_fullMapExtraWalk = false;
    precull = keep;
    const DWORD used = (*reinterpret_cast<const DWORD*>(ci + 0x4C) - *reinterpret_cast<const DWORD*>(ci + 0x48)) / 12;
    const bool full = *reinterpret_cast<const DWORD*>(ci + 0x4C) >= *reinterpret_cast<const DWORD*>(ci + 0x50);
    static DWORD lastLog = 0; const DWORD now = GetTickCount();
    if (now - lastLog >= 10000) {
        lastLog = now;
        Log("[capture] %d sections, %d instances offered, draw list %u entries%s", secs, insts, used, full ? " - FULL (raise Pools > DrawListPercent)" : "");
    }
}

void __stdcall DrawVisibleSectionsFull(BYTE* ci)
{
    short list[1024];
    SyncCapture();
    const bool player = (*reinterpret_cast<const DWORD*>(ci + 0x44) & 0x6000) != 0;
    const bool ours = g_enabled && g_rules && !(g_cfg.profile == PROFILE_REMIX && !g_captureOn);   // Remix without capture: vanilla
    // camera outside every drivable zone (free camera tools, high replay cameras): vanilla then lists every loaded xx00..xx09
    // section without panoramas and our rules had no zone - black ground / sea (2026-10-17, Camera Toolkit). Instead the
    // last zone the camera was in is used as if the camera were still there, and precull is off (its cells only hold
    // data for positions on the road).
    const BYTE* view = *reinterpret_cast<const BYTE* const*>(ci + 0x40);
    const BYTE* cam = view ? *reinterpret_cast<const BYTE* const*>(view + 0x40) : nullptr;
    const BYTE* zone = cam ? static_cast<const BYTE*>(kFindDrivable(reinterpret_cast<void*>(0x00883E70), reinterpret_cast<const float*>(cam + 0x40))) : nullptr;
    const bool outside = ours && cam && !zone && g_lastZone;
    int& precullListed = *reinterpret_cast<int*>(ci + 0x84);
    const int precullKeep = precullListed;
    int n;
    if (outside) { zone = g_lastZone; n = ZoneList(zone, list, 1024); precullListed = -1; ++g_stats.noZone; }
    else {
        n = kGetVisible(list, 1024, ci);             // vanilla list (and its side effects), no 128 limit
        if (zone && ours) g_lastZone = zone;
    }
    const bool flareView = player && *reinterpret_cast<const DWORD*>(ci + 0x40) == reinterpret_cast<DWORD>(kGetView(1));
    if (flareView) g_flareBuild = 0;
    struct FlareCommit { bool on; bool valid; ~FlareCommit() { if (on) { g_flareN = g_flareBuild; g_flareValid = valid; } } } flareCommit{ flareView, ours && (g_cfg.fullMapExtras != 0 || g_captureOn) };
    if (g_captureOn && player) { precullListed = precullKeep; ++g_stats.views; DrawCapture(ci); return; }
    const bool extrasOn = g_cfg.fullMapExtras != 0;
    const bool replace = ours && g_replace && extrasOn && player;   // never hide a panorama without its real sections
    // BooBoo cells (0x8832C0, 128x128 bits of 32 m): vanilla switches precull off there for every view; with the BooBoo map
    // ignored (lod.cpp PrecullBooBooOff) EA's broken precull data of those cells hid the panoramas - black holes in the
    // distance. Panorama sections keep the vanilla behaviour in those cells (no precull), real sections keep precull.
    bool booCell = false;
    if (g_cfg.boobooOff) {
        const float* camPos = reinterpret_cast<const float*>(ci + 0x60);
        const int bx = (static_cast<int>(camPos[0]) >> 5) & 0x7F, by = (static_cast<int>(camPos[1]) >> 5) & 0x7F;
        const int bit = (by << 7) + bx;
        booCell = (reinterpret_cast<const BYTE*>(0x008832C0)[bit >> 3] >> (bit & 7)) & 1;
    }
    for (int i = 0; i < n; ++i) {
        const int s = list[i];
        if (replace && s >= 0 && s < kMaxSection && g_isDrop[s]) continue;   // city panorama -> its real sections below
        BYTE* h = SectionHeader(s);
        if (!h) continue;
        if (flareView) FlareAdd(s);
        if (g_guardOn && player) {
            const float near2 = static_cast<float>((std::max)(g_cfg.guardRadius, g_cfg.precullOffRadius));
            const float* root = *reinterpret_cast<const float* const*>(h + 0x28);
            if (BoxDist2(root, root + 3, reinterpret_cast<const float*>(ci + 0x60)) <= near2 * near2) { GuardWalk(h, ci); continue; }
        }
        if (booCell && s >= 0 && s < kMaxSection && g_isPan[s]) {
            const int keep = precullListed; precullListed = -1; kWalk(h, ci); precullListed = keep;
        } else kWalk(h, ci);
    }
    ++g_stats.views;
    if (g_guardOn && player) {
        static DWORD lastGuardLog = 0; const DWORD now = GetTickCount();
        if (now - lastGuardLog >= 10000) { lastGuardLog = now; Log("[guard] %d instances inside %d m of the camera kept (no frustum / occlusion / pixel cull)", g_guardInst, g_cfg.guardRadius); }
        g_guardInst = 0;
    }
    struct RestorePrecull { int& p; int v; ~RestorePrecull() { p = v; } } restore{ precullListed, precullKeep };
    if (!ours || !extrasOn) return;                                         // extra sections off / Remix without capture: exactly vanilla
    if (!player) return;                                                    // extras only in player views
    if (!zone) return;                                                      // outside every zone before any zone was seen: vanilla only
    const int zoneNum = *reinterpret_cast<const short*>(zone + 8);
    BuildBlocked(zoneNum);
    memset(g_listed, 0, sizeof(g_listed));
    for (int i = 0; i < n; ++i) if (list[i] >= 0 && list[i] < kMaxSection) g_listed[list[i]] = 1;
    int& precull = *reinterpret_cast<int*>(ci + 0x84);
    const int saved = precull;
    if (!g_cfg.extraPrecull) precull = -1;   // precull on: the camera cell's bits, as for the listed sections (the main performance lever)
    g_fullMapExtraWalk = true;
    const LARGE_INTEGER tExtras = PerfNow();
    int extras = 0, nodes = 0, dups = 0;
    const bool batch = BatchBegin(ci);            // Vanilla, main player view: merged meshes + the instances they leave out
    memset(g_walked, 0, sizeof(g_walked));
    const BYTE* head = reinterpret_cast<const BYTE*>(0x0088EAD4);
    for (const BYTE* node = *reinterpret_cast<const BYTE* const*>(head); node && node != head && nodes < 8192; node = *reinterpret_cast<const BYTE* const*>(node)) {
        ++nodes;
        int s = *reinterpret_cast<const int*>(node + 0x0C);
        if (s < 0 || s >= kMaxSection || s / 100 == 26) continue;
        if (g_walked[s]) { ++dups; continue; }   // the same section linked twice (re-activation) must not be drawn twice
        g_walked[s] = 1;
        if (g_listed[s] || g_isPan[s] || g_isNever[s] || g_blocked[s]) continue;
        if (BYTE* h = SectionHeader(s)) {
            if (!(batch && BatchTakeSection(s, h, ci))) kWalk(h, ci);
            if (flareView) FlareAdd(s);
            ++extras;
        }
    }
    if (batch) BatchEmit(ci);
    g_fullMapExtraWalk = false;
    PerfExtrasAdd(PerfMsSince(tExtras));
    precull = saved;
    g_stats.zone = zoneNum; g_stats.listed = n; g_stats.extras = extras;
    static DWORD lastLog = 0; DWORD now = GetTickCount();
    if (now - lastLog >= 10000) {
        lastLog = now;
        Log("[fullmap] zone %d%s: vanilla list %d, extras %d, loaded-list nodes %d, duplicate links skipped %d, camera outside every zone %u views so far",
            zoneNum, outside ? " (camera outside, last zone)" : "", n, extras, nodes, dups, g_stats.noZone);
    }
}

// call 0x57FA11: TheStreamingTrafficCarManager step, then everything wanted is read and activated before the start
typedef void (__thiscall* Plain_t)(void* self);
const Plain_t kTrafficStep = reinterpret_cast<Plain_t>(0x00420030);
const Plain_t kBlockUntilLoaded = reinterpret_cast<Plain_t>(0x005E79D0);
void __fastcall LoaderStepHook(void* traffic, void*)
{
    kTrafficStep(traffic);
    if (!g_enabled || !g_rules) return;
    g_lastZone = nullptr;                          // new load: zone pointers of the old data are gone
    if (!WantAll()) return;                        // Remix without capture: vanilla streaming
    DWORD t0 = GetTickCount();
    kBlockUntilLoaded(reinterpret_cast<void*>(0x0088EB00));
    const BYTE* ts = reinterpret_cast<const BYTE*>(0x0088EB00);
    Log("full map: loading screen activated everything in %u ms (wanted %d, activated %d, skipped %d)", GetTickCount() - t0,
        *reinterpret_cast<const int*>(ts + 0x298), *reinterpret_cast<const int*>(ts + 0x24), *reinterpret_cast<const int*>(ts + 0x28));
    BatchBuild(g_isPan, g_isNever);
}
// 5. Slot pools: the world pools have fixed counts (bMalloc 0x43F8F0 never grows; flags 0) and the game does not check
//    for null everywhere - with the whole region loaded TexturePackSlotPool (128) ran out and InternalLoadTexturePack
//    HeaderChunks 0x494380 crashed at 0x491A00 (2026-10-17). bNewSlotPool 0x440DF0 is hooked once and the world pools
//    get room for a whole region; eModel exhaustion would silently drop LOD models (T72 §1.1), so it gets the most.
struct PoolGrow { const char* name; int minCount; };
const PoolGrow kPoolGrow[] = {
    { "TexturePackSlotPool",            2048 },   // 128
    { "eStreamingPackSlotPool",         1024 },   // 64
    { "eModelSlotPool",                65536 },   // 5000
    { "eTextureBucketSlotPool",        32768 },   // 1024
    { "eMeshRender",                  262144 },   // 4096 (Remix capture: every part of every instance in one frame)
    { "eLightMaterialPlatInfoSlotPool", 2048 },   // 220
    { "eAnimTextureSlotPool",           2048 },   // 256
    { "TopologyTreeSlotPool",           1024 },   // per region
    { "WorldModelSlotPool",             1024 },   // 160
    { "SpaceNodeSlotPool",              1024 },   // 224
};
typedef void* (__cdecl* NewSlotPool_t)(int size, int count, const char* name, int flags);
NewSlotPool_t g_origNewSlotPool = nullptr;
void* __cdecl NewSlotPoolHook(int size, int count, const char* name, int flags)
{
    if (g_cfg.fullMap && name) {
        for (const PoolGrow& g : kPoolGrow)
            if (strcmp(name, g.name) == 0 && count < g.minCount) {
                Log("full map: slot pool %s %d -> %d (%d KB)", name, count, g.minCount, size * g.minCount / 1024);
                count = g.minCount;
                break;
            }
    }
    return g_origNewSlotPool(size, count, name, flags);
}
// 6. Section slots: TheVisibleSectionManager 0x883E70 has 512 slots (0x1C B each, inline, built by its static init
//    0x5E6360) on a free list with the sentinel at +0x6414 (0x88A284); sub_5DB680 takes one per activated section
//    without an empty check - L4RA has 606 sections, the 513th "slot" was the sentinel itself (crash 0x5DB6A3,
//    2026-10-17). Slots are only ever reached through that list and the slot table 0x883EC0 (never by index), so ours
//    are appended to its tail, once, on the first track load (after the static init has built the list).
struct SectionSlotNode { SectionSlotNode* next; SectionSlotNode* prev; DWORD rest[5]; };
static_assert(sizeof(SectionSlotNode) == 0x1C, "section slot");
SectionSlotNode g_slots[2048];
bool g_slotsAdded = false;
void AddSectionSlots()
{
    if (g_slotsAdded) return;
    g_slotsAdded = true;
    SectionSlotNode* sentinel = reinterpret_cast<SectionSlotNode*>(0x0088A284);
    for (SectionSlotNode& n : g_slots) {
        SectionSlotNode* tail = sentinel->prev;
        tail->next = &n; n.prev = tail; n.next = sentinel; sentinel->prev = &n;
    }
    Log("full map: %d section slots of ours appended (512 + %d)", static_cast<int>(sizeof(g_slots) / sizeof(g_slots[0])), static_cast<int>(sizeof(g_slots) / sizeof(g_slots[0])));
}
} // namespace

bool g_fullMapExtraWalk = false;
bool g_guardWalk = false;   // lod.cpp: the instance lies inside the guard radius (no pixel limit)

// lod.cpp BuildWantedHook: should every unit of the region be wanted?
bool FullMapWantAll()
{
    if (!g_enabled) return false;
    AddSectionSlots();
    PickRegion();
    return WantAll();
}

void FullMapInstall()
{
    if (!g_cfg.fullMap) { Log("full map: off"); return; }
    g_replace = g_cfg.fullMapReplace != 0;
    PoolInfo pi = PoolsInfo();
    if (!pi.streamerLimits) { Log("[SKIP] full map: needs the streamer limits (Pools > StreamerLimits)"); return; }
    if (g_cfg.streamPoolPct < 200) Log("[WARN] full map: streaming pool %d %% - L4RA needs about 101 MB, 200 %% gives 117 MB", g_cfg.streamPoolPct);

    static const BYTE drawOrig[6] = { 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
    int hd = PatchAddJmp("full map: DrawVisibleSections", "fullmap", 0x005E99C0, drawOrig, 6, reinterpret_cast<const void*>(&DrawVisibleSectionsFull),
                         "vanilla list + every other loaded real section (T92 rule)");
    static const BYTE stepOrig[5] = { 0xE8, 0x1A, 0x06, 0xEA, 0xFF };
    BYTE stepNew[5] = { 0xE8 }; *reinterpret_cast<DWORD*>(stepNew + 1) = reinterpret_cast<DWORD>(&LoaderStepHook) - (0x0057FA11 + 5);
    int hs = PatchAdd("full map: activate in loading screen", "fullmap", 0x0057FA11, stepOrig, stepNew, 5, "BlockUntilLoaded after loader step 7");
    const DWORD rep0 = 0x171E8, rep0New = 0x181E8, rep1 = 0x17000, rep1New = 0x18000;
    int hr0 = PatchAdd("full map: replay buffer alloc", "fullmap", 0x0057908F, &rep0, &rep0New, 4, "+4 KB: one byte per wanted drivable unit");
    int hr1 = PatchAdd("full map: replay buffer size", "fullmap", 0x005790AB, &rep1, &rep1New, 4, "+4 KB (data starts at +0x1E8)");
    const int hs4[4] = { hd, hs, hr0, hr1 };
    for (int h : hs4) if (!PatchAt(h) || PatchAt(h)->state != PS_Off) { Log("[SKIP] full map: a site is not original - off"); return; }
    static const BYTE poolOrig[8] = { 0x8B, 0x44, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x0C };
    int hp = PatchAddHook("full map: world slot pools", "fullmap", 0x00440DF0, poolOrig, 8, reinterpret_cast<void*>(&NewSlotPoolHook),
                          reinterpret_cast<void**>(&g_origNewSlotPool), "bNewSlotPool: world pools sized for a whole region");
    if (hp < 0 || !PatchIsOn(hp)) { Log("[SKIP] full map: slot pool hook failed - off (pools would overflow)"); return; }
    // 7. Vertex buffers at load, not at first sight: the mesh loader 0x5CC280 defers CreateVertexBuffer to the first draw
    //    (FlushRenderLists 0x5CF5AA: create + lock + copy) while RaceCoordinator is in state 4 - with the whole map those
    //    creations land mid-drive, hundreds at once (measured 2026-10-17: 250-336 ms frames inside RenderFrame, outside the
    //    draw submission). jnz -> jmp at 0x5CC3BD: always created while loading, i.e. in the loading screen.
    static const BYTE vbOrig[2] = { 0x75, 0x1A }, vbNew[2] = { 0xEB, 0x1A };
    PatchSet(PatchAdd("full map: vertex buffers at load", "fullmap", 0x005CC3BD, vbOrig, vbNew, 2, "mesh loader never defers CreateVertexBuffer to the first draw"), true);
    for (int h : hs4) PatchSet(h, true);
    BatchInstall();
    static const BYTE flareOrig[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    PatchAddHook("world flares of every drawn section", "fullmap", 0x00491860, flareOrig, 6, reinterpret_cast<void*>(&WorldFlaresHook),
                 reinterpret_cast<void**>(&g_origWorldFlares), "RenderWorldFlares: the sections drawn (extras too), not only the camera zone list");
    static const BYTE pxOrig[3] = { 0x83, 0xF8, 0x01 }, pxNew[3] = { 0x83, 0xF8, 0xFF };
    g_hCapturePx = PatchAdd("capture: tiny / behind-camera objects", "fullmap", 0x005D6646, pxOrig, pxNew, 3,
                            "AddInstance: px <= 1 cull off (only while Remix capture is on)");
    g_enabled = true;
    Log("[OK] full map on: everything of the region loaded and activated before the start, extras by the T92 rule");
}

void FullMapApplyLive()
{
    const bool r = g_cfg.fullMapReplace != 0;
    if (r == g_replace) return;
    g_replace = r; g_blockedZone = -1;
    char keep[16]; memcpy(keep, g_regionName, sizeof(keep)); g_regionName[0] = 1;   // force PickRegion to rebuild the sets
    PickRegion();
    (void)keep;
}

bool FullMapCapture() { return g_enabled && g_rules && g_cfg.profile == PROFILE_REMIX && g_cfg.captureAll != 0; }

FullMapStats FullMapGet() { return g_stats; }
const char* FullMapRegion() { return g_rules ? g_regionName : (g_regionName[0] ? "no rules" : "-"); }
