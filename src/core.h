// NFSU2 ForceLOD 2 - shared declarations of all modules.
//
// Target: vanilla NFSU2 and RTX Remix only (plain IDirect3DDevice9). Every byte this plugin writes
// into SPEED2.EXE goes through the patch register (patch.cpp): written only over the original bytes,
// switchable live, listed in the menu. Plan: docs/PLAN_FL2.md.
#pragma once
#include <Windows.h>

#define FL2_VERSION "1.0.0"

// ---------------------------------------------------------------------------------------------
// Log (core.cpp)
// ---------------------------------------------------------------------------------------------
void Log(const char* fmt, ...);
extern char g_baseDir[MAX_PATH];   // folder of the .asi (INI, log, dumps)

// ---------------------------------------------------------------------------------------------
// Patch register (patch.cpp)
// ---------------------------------------------------------------------------------------------
enum PatchState {
    PS_Off = 0,      // original bytes in place, ours not applied
    PS_On,           // our bytes in place, written by us
    PS_Shared,       // our bytes already in place, written by another plugin (identical patch) - we never revert it
    PS_Foreign,      // neither original nor ours: another plugin owns the site - left alone
    PS_Failed,       // VirtualProtect failed
};
struct Patch {
    char  id[48];
    char  module[12];
    DWORD addr;
    int   len;
    BYTE  orig[16];
    BYTE  repl[16];
    int   state;
    char  note[96];
    bool  hook;      // MinHook entry: switched through MinHook, never by rewriting bytes
};
// Registers a site and checks its current bytes. Returns a handle (>= 0) or -1 (register full / bad length).
int  PatchAdd(const char* id, const char* module, DWORD addr, const void* orig, const void* repl, int len, const char* note);
// Same, the replacement is a 5-byte JMP to `target` padded with NOPs to `len`.
int  PatchAddJmp(const char* id, const char* module, DWORD addr, const void* orig, int len, const void* target, const char* note);
// Inline hook through MinHook, registered like a patch (switchable). `orig` = expected first bytes (len 5..8).
int  PatchAddHook(const char* id, const char* module, DWORD addr, const void* orig, int len, void* detour, void** trampoline, const char* note);
bool PatchSet(int h, bool on);                 // apply / revert (only over the bytes we expect)
bool PatchSetRepl(int h, const void* repl);    // new replacement bytes (live values); rewritten at once while on
bool PatchIsOn(int h);                         // on or shared: our bytes are in effect
int  PatchCount();
const Patch* PatchAt(int i);
bool WriteCode(DWORD addr, const void* src, int len);

// ---------------------------------------------------------------------------------------------
// Configuration (config.cpp)
// ---------------------------------------------------------------------------------------------
enum Profile { PROFILE_REMIX = 0, PROFILE_VANILLA = 1 };

enum HiddenSetting {
    HS_CarShadow, HS_CarHeadlight, HS_Crowd, HS_Fog, HS_MotionBlur, HS_LightStreaks, HS_LightGlow,
    HS_AnimatedTexture, HS_ParticleSystem, HS_DepthOfField, HS_OverBright, HS_BleachByPass, HS_Tinting,
    HS_HorizonFog, HS_Rain,
    HS_Count
};
struct HiddenSettingDef { const char* key; DWORD addr; int maxValue; const char* help; };
extern const HiddenSettingDef kHidden[HS_Count];

struct Config {
    int profile = PROFILE_REMIX;
    // [LOD] - live unless noted
    int worldMinPx      = 4;    // scenery below N px (game's own pixel scale, vanilla 17) is not drawn
    int worldPxHyst     = 2;    // px band above the limit that keeps the previous decision (no flicker at the edge)
    int reflectionLodA  = 1;    // reflection / env views also draw LOD A (vanilla: B/C)
    int carLod          = 0;    // car body LOD override (0 = A, -1 = game)
    int carTyreLod      = 0;    // tyre LOD override (0 = A, -1 = game)
    int cullableParts   = 1;    // underbody / brakes / side parts never culled by camera angle
    int carMinPx        = 1;    // cars below N px are not drawn (vanilla 4)
    int carLowDetailPx  = 0;    // below N px cars lose wheels/brakes/attachments (vanilla 18, 0 = never)
    int carBlinkOff     = 1;    // respawn blinking (car hidden every 2nd frame) off
    int boobooOff       = 1;    // live: the precull BooBoo map (cells where the game switches precull off) is ignored
    int lodaUnits       = 1;    // startup: keep every X/Y unit < 50 (shared LOD A solids + textures) loaded
    int fullMap         = 1;    // startup: whole region loaded, real sections drawn instead of the far stand-ins (T92 rule)
    int fullMapExtras   = 1;    // live: draw every other loaded real section too (0 = the whole map stays loaded, drawn like vanilla)
    int extraPrecull    = 0;    // live: the far sections use the game's precull too (50-90 % fewer objects; T91 6: some bits beyond 1024 m are wrap noise)
    int captureAll      = 0;    // live, Remix profile only: capture - whole region loaded, EVERY object drawn (no occlusion, no frustum, no pixel limit) except panoramas; off = Remix draws exactly vanilla (LOD A)
    int fullMapReplace  = 1;    // live: city panoramas covered by real geometry are replaced by those real sections
    int guardRadius     = 0;    // live, Remix profile: objects within N m of the camera are never culled - not by view frustum, occlusion or pixel size (beside / behind the camera too: Remix lights and shadows)
    int precullOffRadius= 0;    // live, Remix profile: no occlusion (precull) culling within N m of the camera
    int extraMinPx      = 0;    // live: own pixel limit for the extra (far) sections, 0 = same as the world limit (everything)
    int staticBatching  = 0;    // Vanilla profile only: far sections merged into static meshes in the loading screen (built at load, drawing live)
    int batchTrees      = 1;    // next load: swaying trees batched too, drawn per instance (swaying) only within 200 m
    int batchBudgetMB   = 128;  // next load: upper limit for the merged buffers (VRAM + the managed copy in RAM)
    // [Despawn] - live, both profiles
    int trafficDespawn  = 400;  // m: traffic teleported away / encounter cars deactivated beyond this (vanilla 250)
    int flareDistance   = 400;  // m: light flares fade out up to this distance (vanilla 200)
    // [Reflections] - startup, Vanilla profile only (sizes of the game's reflection render targets)
    int roadReflScale   = 1;    // road reflection 320x240 x 1 / 2 / 4
    int carEnvMapSize   = 0;    // car env-map cube face: 0 = vanilla 128, 256, 512
    int mirrorSize      = 0;    // rear-view mirror: 0 = vanilla 256, 512, 1024
    // [Pools] - startup
    int streamPoolPct   = 200;  // streaming pool (vanilla 100; above 200 city lights vanish after a shop visit)
    int packPoolPct     = 200;  // car loader pack pool
    int polyBufPct      = 400;  // ePoly / FrameMalloc buffers (matrices for every drawn instance and car)
    int fengPoolPct     = 100;  // front end pools
    int drawListPct     = 400;  // scenery draw list (vanilla 4200 entries for all views together)
    int laaFix          = 1;    // unsigned pointer compares (needed with a LAA exe, RTX Remix, big pools)
    int streamerLimits  = 1;    // stream heap +1024 block nodes, wanted / deferred lists 256 / 64 -> 1024 (docs T91 §2/§3)
    // [Memory] - startup: fixes for crashes of the game itself
    int heapGuard       = 1;    // bFree: a block with a dead list neighbour is logged and freed without the crash (track unload)
    int poolLock        = 1;    // bMalloc pool race: allocation / free from two threads corrupt the pool (multi-core crashes)
    int fileGuard       = 1;    // file request list: empty list after quick shop visits -> crash 0x6F6AF9
    int lowMemD3dx      = 1;    // LAA: the game's D3DX takes pointers above 2 GB for handles -> its allocations below 2 GB
    // [Settings] -1 = game menu, 0/1(/2) forced - live
    int hidden[HS_Count];
    // [General]
    int menuKey         = VK_INSERT;
    int log             = 1;
    int crashLog        = 1;
    int hangWatchdog    = 30;   // s in the loading state with the streamer waiting -> log (0 = off)
    int inputBlock      = 1;
    char inputGuardModules[256] = "NFSU_XtendedInput";
    Config() { for (int i = 0; i < HS_Count; ++i) hidden[i] = -1; }
};
extern Config g_cfg;
void ConfigDefaults(Config& c, int profile);   // profile start values
void ConfigLoad();
void ConfigSwitchProfile(int profile);         // menu: profile values + that profile's [Settings.<profile>]
void ConfigSave();
const char* ProfileName(int p);

// ---------------------------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------------------------
// lod.cpp
void LodInstall();
void LodApplyLive();
void LodFrame();          // once per frame on the render thread (EndScene): turns the LOD counters into per-frame values
struct LodCounters {      // per frame
    unsigned drawnA, drawnNoA, refBC, unboundCulled, pixelCulled, listFull, bound, bindMissing;
};
LodCounters LodLastFrame();
bool LodHooked();

// pools.cpp
void PoolsInstall();
struct PoolInfo { DWORD drawListEntries; DWORD drawListBase; bool exeLaa; bool streamerLimits; int extraNodes; };
int  StreamHeapFreeNodes();   // free block nodes of the current stream heap (-1 = none)

// fullmap.cpp
void FullMapInstall();
bool FullMapWantAll();
extern bool g_fullMapExtraWalk;
void FullMapApplyLive();
struct FullMapStats { unsigned views, noZone; int zone, listed, extras; };
FullMapStats FullMapGet();
const char* FullMapRegion();

// batch.cpp - static batching of the extra sections (docs/BATCHING_SPEC.md)
bool BatchInstall();
void BatchBuild(const BYTE* isPan, const BYTE* isNever);   // loading screen, after BlockUntilLoaded
bool BatchBegin(BYTE* ci);                                 // DrawVisibleSectionsFull: this view batches
bool BatchTakeSection(int s, BYTE* hdr, BYTE* ci);         // true = section drawn by batches + its leftover instances
void BatchEmit(BYTE* ci);                                  // batch entries into the visibility list
struct BatchStats { int meshes; float mb; DWORD buildMs; int batchedInstances, emitted, distCulled, frustumCulled, leftoverCalls, activeParts,
                    partRebuilds, deadMeshes, fallbackSections, staleSections, listFull, nearTreeGroups; };
BatchStats BatchGet();
const BYTE* LodModelForBatch(const BYTE* info);
bool FullMapCapture();
extern bool g_guardWalk;                                   // fullmap.cpp: the instance lies inside the guard radius (lod.cpp: no pixel limit)            // lod.cpp: the model lod.cpp draws for this info (A bound on the spot), 0 = none

// despawn.cpp
void DespawnInstall();
void DespawnApplyLive();

// reflections.cpp
void ReflectionsInstall();
const char* ReflectionsStatus();

// memfix.cpp
void MemFixInstall();
struct MemFixStats { volatile long otherThreadAllocs, otherThreadFrees, heapGuardHits, fileWaits, fileRefused; int fileMinFree; };
MemFixStats MemFixGet();
void MemFixFrame();
PoolInfo PoolsInfo();

// settings.cpp
void SettingsInstall();
void SettingsApplyLive();
void SettingsAfterDevice();   // ui.cpp: right after CreateDevice, before eCreateRenderTargets

// diag.cpp
void DiagInstall(HMODULE self);
void DiagFrame();
const char* DiagRemixStatus();   // "RTX Remix detected" / "no RTX Remix"
bool DiagRemixPresent();
int DiagWarningCount();          // Remix setup problems found at startup (dxvk.conf shaderModel, LAA, profile)
const char* DiagWarning(int i);

// perf.cpp - frame time breakdown in the log
void PerfInstall();
LARGE_INTEGER PerfNow();
double PerfMsSince(const LARGE_INTEGER& t0);
void PerfExtrasAdd(double ms);
void PerfD3D(int which, double ms);   // 0 EndScene, 1 Present
void PerfDraw(unsigned prims);
enum { PERF_STAGES = 14 };            // limiter sim feng cam sound stream res zones render | display cull extras endscene present (nested in render)
struct PerfSummary {                  // the last 10 s window of free driving / racing (the [perf] log line)
    bool valid; int frames; double avgMs, maxMs, otherMs, stageMs[PERF_STAGES];
    unsigned drawsAvg, drawsMax, primsAvg; int zoneSwitches; DWORD tick;
};
PerfSummary PerfLast();
const char* PerfStageName(int s);

// ui.cpp
void UiInstall(HMODULE self);
