// Full frame time breakdown, logged every 10 s while driving and at once for every frame over 50 ms.
// Nothing to do for the player: it runs along. Every MainLoop 0x5811C0 stage is timed with QueryPerformanceCounter
// (MinHook entry hooks that only take two timestamps), plus the render internals and the D3D9 side:
//   limiter   FrameLimiter_Wait 0x57EAD0           sim     World::DoTimestep 0x609CE0
//   feng      FEngUpdate 0x5663B0                   cam     UpdateCameraMovers 0x44EFE0
//   sound     EAXSound::Update 0x483B40             stream  TrackStreamer::ServiceNonGameState 0x5E79A0 + HandleLoading 0x5E6A70
//   res       ServiceResourceLoading 0x57EF60       zones   SwitchZones 0x5E7AF0 (count + time)
//   render    RenderFrame 0x5D2850 (call 0x5814D6)  display eDisplayFrame 0x5CAAD0  cull BuildCullLists 0x5B9C20
//   extras    fullmap far sections (inside cull)    endscene / present: D3D9 calls (present = waiting for the GPU / vsync)
//   draws     DrawIndexedPrimitive / DrawPrimitive calls and primitives per frame
//   other     frame - all of the above (input, AI outside DoTimestep, Win32 messages ...)
#include "core.h"
#include <cstring>
#include <cstdio>

void PerfFrameEnd();

namespace {
LARGE_INTEGER g_freq = {};
enum Stage { S_Limiter, S_Sim, S_FEng, S_Cam, S_Sound, S_Stream, S_Res, S_Zones, S_Render, S_Display, S_Cull, S_Extras, S_EndScene, S_Present, S_Count };
const char* kStageName[S_Count] = { "limiter", "sim", "feng", "cam", "sound", "stream", "res", "zones", "render", "display", "cull", "extras", "endscene", "present" };
double g_sum[S_Count] = {}, g_cur[S_Count] = {};
double g_frameSum = 0, g_frameMax = 0;
int    g_frames = 0, g_zoneCalls = 0, g_curZoneCalls = 0, g_spikeLogs = 0;
unsigned g_draws = 0, g_prims = 0, g_curDraws = 0, g_curPrims = 0, g_maxDraws = 0;
LARGE_INTEGER g_lastFrame = {};
DWORD  g_lastLog = 0;
bool   g_on = false;

inline LARGE_INTEGER Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }
inline void Add(int s, const LARGE_INTEGER& t0) { LARGE_INTEGER t = Now(); g_cur[s] += static_cast<double>(t.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart); }

typedef void (__cdecl*    VoidF_t)(float);
typedef void (__fastcall* ThisF_t)(void*, void*, float);
typedef void (__cdecl*    Void_t)();
typedef void (__fastcall* This_t)(void*, void*);
typedef char (__fastcall* ThisC_t)(void*, void*);
typedef void (__fastcall* ThisP_t)(void*, void*, void*);
typedef int  (__cdecl*    Cull_t)(void*);

VoidF_t o_limiter; ThisF_t o_sim; Void_t o_feng; Void_t o_cam; ThisF_t o_sound; This_t o_nongame; ThisC_t o_loading;
Void_t o_res; ThisP_t o_zones; Void_t o_display; Cull_t o_cull;

void __cdecl  H_limiter(float a)                 { auto t = Now(); o_limiter(a); Add(S_Limiter, t); }
void __fastcall H_sim(void* c, void* e, float a)  { auto t = Now(); o_sim(c, e, a); Add(S_Sim, t); }
void __cdecl  H_feng()                           { auto t = Now(); o_feng(); Add(S_FEng, t); }
void __cdecl  H_cam()                            { auto t = Now(); o_cam(); Add(S_Cam, t); }
void __fastcall H_sound(void* c, void* e, float a){ auto t = Now(); o_sound(c, e, a); Add(S_Sound, t); }
void __fastcall H_nongame(void* c, void* e)       { auto t = Now(); o_nongame(c, e); Add(S_Stream, t); }
char __fastcall H_loading(void* c, void* e)       { auto t = Now(); char r = o_loading(c, e); Add(S_Stream, t); return r; }
void __cdecl  H_res()                            { auto t = Now(); o_res(); Add(S_Res, t); }
void __fastcall H_zones(void* c, void* e, void* p){ auto t = Now(); o_zones(c, e, p); Add(S_Zones, t); ++g_curZoneCalls; }
void __cdecl  H_display()                        { auto t = Now(); o_display(); Add(S_Display, t); }
int  __cdecl  H_cull(void* a)                    { auto t = Now(); int r = o_cull(a); Add(S_Cull, t); return r; }

// RenderFrame through its call site in MainLoop (its entry is the hidden-settings hook of settings.cpp)
const Void_t kRenderFrame = reinterpret_cast<Void_t>(0x005D2850);
void __cdecl H_render() { auto t = Now(); kRenderFrame(); Add(S_Render, t); PerfFrameEnd(); }

void Hook(const char* id, DWORD addr, const BYTE* orig, int len, void* detour, void** tramp)
{
    PatchAddHook(id, "perf", addr, orig, len, detour, tramp, "frame time measurement (timestamps only)");
}

PerfSummary g_last = {};

void Report(double frameMs, bool spike)
{
    char line[900]; int n = 0;
    const double div = spike ? 1.0 : static_cast<double>(g_frames);
    const double* v = spike ? g_cur : g_sum;
    double known = 0;
    for (int s = 0; s < S_Count; ++s) {
        // nested stages are not added to "known": display/cull/extras/endscene/present run inside render
        if (s != S_Display && s != S_Cull && s != S_Extras && s != S_EndScene && s != S_Present) known += v[s] / div;
        n += snprintf(line + n, sizeof(line) - n, " %s %.2f", kStageName[s], v[s] / div);
    }
    const double frame = spike ? frameMs : g_frameSum / div;
    if (!spike) {
        g_last.valid = true; g_last.frames = g_frames; g_last.avgMs = frame; g_last.maxMs = g_frameMax; g_last.otherMs = frame - known;
        for (int s = 0; s < S_Count; ++s) g_last.stageMs[s] = v[s] / div;
        g_last.drawsAvg = g_draws / g_frames; g_last.drawsMax = g_maxDraws; g_last.primsAvg = g_prims / g_frames; g_last.zoneSwitches = g_zoneCalls;
        g_last.tick = GetTickCount();
    }
    if (spike)
        Log("[perf SPIKE] frame %.1f ms |%s | other %.2f | draws %u prims %u zone switches %d", frameMs, line, frame - known, g_curDraws, g_curPrims, g_curZoneCalls);
    else
        Log("[perf] %d frames avg %.2f ms (%.0f fps) max %.1f ms |%s | other %.2f | draws avg %u max %u, prims avg %u | zone switches %d",
            g_frames, frame, 1000.0 / frame, g_frameMax, line, frame - known, g_draws / g_frames, g_maxDraws, g_prims / g_frames, g_zoneCalls);
}
} // namespace

PerfSummary PerfLast() { return g_last; }
const char* PerfStageName(int s) { return s >= 0 && s < S_Count ? kStageName[s] : "?"; }
void PerfExtrasAdd(double ms) { g_cur[S_Extras] += ms; }
double PerfMsSince(const LARGE_INTEGER& t0) { return static_cast<double>(Now().QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart); }
LARGE_INTEGER PerfNow() { return Now(); }
void PerfD3D(int which, double ms) { if (g_on) g_cur[which == 0 ? S_EndScene : S_Present] += ms; }
void PerfDraw(unsigned prims) { ++g_curDraws; g_curPrims += prims; }

void PerfInstall()
{
    static_assert(S_Count == PERF_STAGES, "PerfSummary stage count");
    QueryPerformanceFrequency(&g_freq);
    static const BYTE b_lim[5]  = { 0xA1, 0x4C, 0x51, 0x86, 0x00 };
    static const BYTE b_sim[6]  = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
    static const BYTE b_feng[5] = { 0xB9, 0x40, 0xAD, 0x85, 0x00 };
    static const BYTE b_cam[6]  = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    static const BYTE b_snd[6]  = { 0x51, 0xA1, 0xA8, 0xAA, 0x7F, 0x00 };
    static const BYTE b_ng[5]   = { 0x56, 0x8B, 0xF1, 0xE8, 0xF8 };
    static const BYTE b_load[5] = { 0x53, 0x55, 0x56, 0x8B, 0xF1 };
    static const BYTE b_res[5]  = { 0xA1, 0x58, 0x4F, 0x86, 0x00 };
    static const BYTE b_zone[6] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xE9 };
    static const BYTE b_disp[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
    static const BYTE b_cull[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
    Hook("perf: FrameLimiter_Wait", 0x0057EAD0, b_lim, 5, (void*)&H_limiter, (void**)&o_limiter);
    Hook("perf: World::DoTimestep", 0x00609CE0, b_sim, 6, (void*)&H_sim, (void**)&o_sim);
    Hook("perf: FEngUpdate", 0x005663B0, b_feng, 5, (void*)&H_feng, (void**)&o_feng);
    Hook("perf: UpdateCameraMovers", 0x0044EFE0, b_cam, 6, (void*)&H_cam, (void**)&o_cam);
    Hook("perf: EAXSound::Update", 0x00483B40, b_snd, 6, (void*)&H_sound, (void**)&o_sound);
    Hook("perf: ServiceNonGameState", 0x005E79A0, b_ng, 5, (void*)&H_nongame, (void**)&o_nongame);
    Hook("perf: HandleLoading", 0x005E6A70, b_load, 5, (void*)&H_loading, (void**)&o_loading);
    Hook("perf: ServiceResourceLoading", 0x0057EF60, b_res, 5, (void*)&H_res, (void**)&o_res);
    Hook("perf: SwitchZones", 0x005E7AF0, b_zone, 6, (void*)&H_zones, (void**)&o_zones);
    Hook("perf: eDisplayFrame", 0x005CAAD0, b_disp, 6, (void*)&H_display, (void**)&o_display);
    Hook("perf: BuildCullLists", 0x005B9C20, b_cull, 5, (void*)&H_cull, (void**)&o_cull);
    static const BYTE callOrig[5] = { 0xE8, 0x75, 0x13, 0x05, 0x00 };   // call RenderFrame
    BYTE callNew[5] = { 0xE8 }; *reinterpret_cast<DWORD*>(callNew + 1) = reinterpret_cast<DWORD>(&H_render) - (0x005814D6 + 5);
    PatchSet(PatchAdd("perf: RenderFrame call", "perf", 0x005814D6, callOrig, callNew, 5, "frame time measurement"), true);
    g_on = true;
    Log("perf: frame time measurement on ([perf] every 10 s, [perf SPIKE] for frames over 50 ms)");
}

// end of RenderFrame (MainLoop call site) = one complete frame
void PerfFrameEnd()
{
    if (!g_on) return;
    LARGE_INTEGER now = Now();
    if (g_lastFrame.QuadPart) {
        const double ms = static_cast<double>(now.QuadPart - g_lastFrame.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
        if (ms > 50.0 && *reinterpret_cast<const int*>(0x008654A4) == 6 && g_spikeLogs < 200) { ++g_spikeLogs; Report(ms, true); }
        for (int s = 0; s < S_Count; ++s) g_sum[s] += g_cur[s];
        g_frameSum += ms; if (ms > g_frameMax) g_frameMax = ms; ++g_frames;
        g_draws += g_curDraws; g_prims += g_curPrims; if (g_curDraws > g_maxDraws) g_maxDraws = g_curDraws;
        g_zoneCalls += g_curZoneCalls;
    }
    memset(g_cur, 0, sizeof(g_cur)); g_curDraws = g_curPrims = 0; g_curZoneCalls = 0;
    g_lastFrame = now;
    const DWORD tick = GetTickCount();
    if (tick - g_lastLog < 10000 || g_frames == 0) return;
    g_lastLog = tick;
    if (*reinterpret_cast<const int*>(0x008654A4) == 6) Report(0, false);
    memset(g_sum, 0, sizeof(g_sum)); g_frameSum = g_frameMax = 0; g_frames = 0; g_draws = g_prims = g_maxDraws = 0; g_zoneCalls = 0;
}
