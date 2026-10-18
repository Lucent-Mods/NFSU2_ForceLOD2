// Memory pools and fixed lists - all startup values (the game allocates them once).
//
// Streaming pool: size table 0x79DC54 (60000/43000/36000 KB: [0] city, [1] two players, [2] L4RG). [0] and [1]
//   scaled, [2] stays (docs T92 §9 #1). Max 200 %: above that the city lights vanish after a shop visit (measured).
// Stream heap block nodes (docs T91 §2 #1, §3): the heap has 192 nodes inline (this+0x1C..+0xF1C), taken without an
//   empty check (0x5DA5C1). Enlarging them in place (0x5E4BB2/0x5E4BDE/0x5E4E9D) overlaps the pool override at
//   +0xF1C - the old ForceLOD crash. Instead, after every StreamHeap_ctor (call 0x5E4ED7 in InitMemoryPool) 1024
//   nodes of ours are appended to the free list heap+0x0C, exactly like the ctor appends its own. The dtor 0x5E4D70
//   only empties the sentinels and frees no node, so the array is simply re-used at the next InitMemoryPool.
// Wanted list (256 x 4 B at TrackStreamer+0x29C) and deferred list (64 x 4 B at +0x188) have no bound check
//   (AddSectionsToLoad 0x5DACB0, DeferUnit 0x5DF120); behind them lie the heap pointer +0x69C and the lock bytes
//   +0x288. Moved to 1024-entry arrays of ours through their 14 + 4 disp32 operands (all in TrackStreamer methods,
//   base register always TheTrackStreamer 0x88EB00; docs T91 §2 #2/#3). The counters stay where they are.
// Pack pool: car loader 0x7FA9D4. ePoly / FrameMalloc: 2 x 0x64000, four immediates 0x48CD62/6C/91/A2 -
//   when it runs dry instances and whole cars vanish silently (0x5D6785 / 0x620E1E).
// Front end pools: 0x7F917C (2 x 400000 B).
// Scenery draw list: one list of 4200 x 12 B for all views (0x876DE0..0x8832C0); full -> instance dropped.
//   Moved to an own buffer through the two immediates 0x5E9A35 / 0x5E9A55 (never extended in place:
//   0x8832C0 is the precull BooBoo map).
// LAA: with a LARGE_ADDRESS_AWARE exe pointers above 0x80000000 break signed compares (jl -> jb).
#include "core.h"
#include <cstring>

namespace {
PoolInfo g_info = { 4200, 0x00876DE0, false, false, 0 };

// stream heap node {next, prev, addr, size, used} - layout of the ctor 0x5E4BE2..0x5E4C65
struct HeapNode { HeapNode* next; HeapNode* prev; DWORD addr; DWORD size; BYTE used; BYTE pad[3]; };
static_assert(sizeof(HeapNode) == 0x14, "stream heap node");
constexpr int kExtraNodes = 1024;
HeapNode g_extra[kExtraNodes];
DWORD g_ctorRet = 0;
constexpr DWORD kStreamHeapCtor = 0x005E4B70;
DWORD g_ptrCtor = kStreamHeapCtor;

void __cdecl AddNodes(BYTE* heap)
{
    if (!heap) return;
    HeapNode* sentinel = reinterpret_cast<HeapNode*>(heap + 0x0C);
    for (int i = 0; i < kExtraNodes; ++i) {
        HeapNode* n = &g_extra[i];
        HeapNode* tail = sentinel->prev;
        n->addr = 0; n->size = 0; n->used = 0;
        tail->next = n; sentinel->prev = n; n->next = sentinel; n->prev = tail;
    }
    g_info.extraNodes = kExtraNodes;
    Log("stream heap %p: %d block nodes of ours appended (192 + %d)", heap, kExtraNodes, kExtraNodes);
}

// replaces `call StreamHeap_ctor` at 0x5E4ED7: thiscall, 4 args, ret 10h, returns this in eax
__declspec(naked) void CtorStub()
{
    __asm {
        pop  g_ctorRet
        call dword ptr [g_ptrCtor]
        push eax
        push eax
        call AddNodes
        add  esp, 4
        pop  eax
        jmp  dword ptr [g_ctorRet]
    }
}

DWORD g_wanted[1024];
DWORD g_deferred[1024];
struct DispSite { DWORD va; int at; };
const DispSite kWantedSites[14] = {
    { 0x5D5ECF, 2 }, { 0x5D60AF, 2 }, { 0x5D61A7, 2 }, { 0x5DA945, 2 }, { 0x5DAC78, 3 }, { 0x5DAD3D, 3 }, { 0x5DAEF6, 2 },
    { 0x5DF16A, 2 }, { 0x5DF18D, 3 }, { 0x5DF5A1, 2 }, { 0x5DF747, 2 }, { 0x5E6276, 2 }, { 0x5E8406, 2 }, { 0x5E84EB, 2 } };
const DispSite kDeferredSites[4] = { { 0x5DA8FF, 2 }, { 0x5DAC61, 2 }, { 0x5DF13F, 3 }, { 0x5E7CF2, 2 } };

void StreamerLimits()
{
    // all or nothing: a half-moved list is worse than none
    int hs[19]; int n = 0; bool ok = true;
    const DWORD wantDisp = reinterpret_cast<DWORD>(g_wanted) - 0x0088EB00, defDisp = reinterpret_cast<DWORD>(g_deferred) - 0x0088EB00;
    const DWORD wantOrig = 0x29C, defOrig = 0x188;
    for (const DispSite& s : kWantedSites)   { hs[n] = PatchAdd("wanted list -> 1024", "pools", s.va + s.at, &wantOrig, &wantDisp, 4, "TrackStreamer+0x29C operand"); ok &= hs[n] >= 0 && PatchAt(hs[n])->state == PS_Off; ++n; }
    for (const DispSite& s : kDeferredSites) { hs[n] = PatchAdd("deferred list -> 1024", "pools", s.va + s.at, &defOrig, &defDisp, 4, "TrackStreamer+0x188 operand"); ok &= hs[n] >= 0 && PatchAt(hs[n])->state == PS_Off; ++n; }
    const BYTE callOrig[5] = { 0xE8, 0x94, 0xFC, 0xFF, 0xFF };   // call 0x5E4B70
    BYTE callNew[5] = { 0xE8 }; *reinterpret_cast<DWORD*>(callNew + 1) = reinterpret_cast<DWORD>(&CtorStub) - (0x005E4ED7 + 5);
    hs[n] = PatchAdd("stream heap +1024 nodes", "pools", 0x005E4ED7, callOrig, callNew, 5, "call StreamHeap_ctor -> ctor + our nodes on the free list");
    ok &= hs[n] >= 0 && PatchAt(hs[n])->state == PS_Off; ++n;
    if (!ok) { Log("[SKIP] streamer limits: not every site is original - none changed"); return; }
    for (int i = 0; i < n; ++i) PatchSet(hs[i], true);
    g_info.streamerLimits = true;
    Log("[OK] streamer limits: wanted %p / deferred %p (1024 each), +%d heap nodes per stream heap", g_wanted, g_deferred, kExtraNodes);
}

bool ExeIsLaa()
{
    const BYTE* img = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

void ScaleTable(const char* id, DWORD addr, const DWORD orig[3], int pct, const char* note)
{
    DWORD repl[3];
    for (int i = 0; i < 3; ++i) repl[i] = static_cast<DWORD>(static_cast<unsigned long long>(orig[i]) * pct / 100);
    int h = PatchAdd(id, "pools", addr, orig, repl, 12, note);
    if (pct > 100) PatchSet(h, true);
}

// LAA sites: VA, original, patched (signed -> unsigned branch on pointer compares).
struct LaaSite { DWORD va; BYTE orig[2]; BYTE repl[2]; const char* what; };
const LaaSite kLaa[] = {
    { 0x0044055E, { 0x7C, 0x09 }, { 0x72, 0x09 }, "LAA: bMemory free, pool override range (lo)" },
    { 0x00440567, { 0x7C, 0x44 }, { 0x72, 0x44 }, "LAA: bMemory free, pool override range (hi)" },
    { 0x005DA4E3, { 0x7C, 0x06 }, { 0x72, 0x06 }, "LAA: stream heap alloc hint (lo)" },
    { 0x005DA4E9, { 0x7C, 0x4E }, { 0x72, 0x4E }, "LAA: stream heap alloc hint (hi)" },
    // within one buffer - only wrong if a single buffer crosses 0x80000000 (docs LAA_AUDIT.md, optional set)
    { 0x00719C5C, { 0x7C, 0xB4 }, { 0x72, 0xB4 }, "LAA: VP6 movie scaler (1)" },
    { 0x00719D30, { 0x7C, 0xA8 }, { 0x72, 0xA8 }, "LAA: VP6 movie scaler (2)" },
    { 0x0076EC86, { 0x0F, 0x8D }, { 0x0F, 0x83 }, "LAA: D3DX MMX box filter (1)" },
    { 0x0076ECA2, { 0x7D, 0x71 }, { 0x73, 0x71 }, "LAA: D3DX MMX box filter (2)" },
    { 0x0076ED13, { 0x7C, 0x8F }, { 0x72, 0x8F }, "LAA: D3DX MMX box filter (3)" },
    { 0x0076ED27, { 0x0F, 0x8C }, { 0x0F, 0x82 }, "LAA: D3DX MMX box filter (4)" },
};
} // namespace

void PoolsInstall()
{
    g_info.exeLaa = ExeIsLaa();
    for (const LaaSite& s : kLaa) {
        int h = PatchAdd(s.what, "pools", s.va, s.orig, s.repl, 2, "signed pointer compare -> unsigned");
        if (g_cfg.laaFix && g_info.exeLaa) PatchSet(h, true);
    }
    if (!g_info.exeLaa) Log("[INFO] SPEED2.EXE is not LARGE_ADDRESS_AWARE: LAA fix not needed");

    {
        static const DWORD kStream[3] = { 60000, 43000, 36000 };
        DWORD repl[3] = { static_cast<DWORD>(60000ull * g_cfg.streamPoolPct / 100), static_cast<DWORD>(43000ull * g_cfg.streamPoolPct / 100), 36000 };
        int h = PatchAdd("streaming pool size", "pools", 0x0079DC54, kStream, repl, 12, "KB: city / two players scaled, L4RG unchanged");
        if (g_cfg.streamPoolPct > 100) PatchSet(h, true);
    }
    if (g_cfg.streamerLimits) StreamerLimits();
    static const DWORD kPack[3] = { 11000, 9000, 16000 };
    ScaleTable("car pack pool size", 0x007FA9D4, kPack, g_cfg.packPoolPct, "KB per region type");

    {   // ePoly / FrameMalloc: four immediates, all or nothing
        const DWORD orig = 0x64000, val = static_cast<DWORD>(0x64000ull * g_cfg.polyBufPct / 100);
        static const DWORD sites[4] = { 0x0048CD62, 0x0048CD6C, 0x0048CD91, 0x0048CDA2 };
        int h[4]; bool allFree = true;
        for (int i = 0; i < 4; ++i) { h[i] = PatchAdd("ePoly/FrameMalloc buffer size", "pools", sites[i], &orig, &val, 4, "2 buffers, matrices of every drawn instance and car"); allFree &= PatchAt(h[i]) && PatchAt(h[i])->state == PS_Off; }
        if (g_cfg.polyBufPct > 100 && allFree) for (int i = 0; i < 4; ++i) PatchSet(h[i], true);
        else if (g_cfg.polyBufPct > 100) Log("[SKIP] ePoly buffers: not all four sites are original - left alone");
    }
    {
        const DWORD orig = 400000, val = static_cast<DWORD>(400000ull * g_cfg.fengPoolPct / 100);
        int h = PatchAdd("front end pool size", "pools", 0x007F917C, &orig, &val, 4, "bytes per FE pool");
        if (g_cfg.fengPoolPct > 100) PatchSet(h, true);
    }
    if (g_cfg.drawListPct > 100) {
        DWORD entries = 4200u * g_cfg.drawListPct / 100;
        BYTE* buf = static_cast<BYTE*>(VirtualAlloc(nullptr, entries * 12, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        const DWORD origStart = 0x00876DE0, origEnd = 0x008832C0;
        DWORD start = reinterpret_cast<DWORD>(buf), end = start + entries * 12;
        int h0 = PatchAdd("draw list start", "pools", 0x005E9A35, &origStart, &start, 4, "scenery draw list moved to an own buffer");
        int h1 = PatchAdd("draw list end",   "pools", 0x005E9A55, &origEnd,   &end,   4, "scenery draw list moved to an own buffer");
        if (buf && PatchAt(h0) && PatchAt(h1) && PatchAt(h0)->state == PS_Off && PatchAt(h1)->state == PS_Off) {
            PatchSet(h0, true); PatchSet(h1, true);
            g_info.drawListEntries = entries; g_info.drawListBase = start;
            Log("[OK] draw list: %u entries (%u KB) @ %p", entries, entries * 12 / 1024, buf);
        } else {
            Log("[SKIP] draw list: buffer %p, sites not original - stays at 4200", buf);
            if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        }
    }
}

PoolInfo PoolsInfo() { return g_info; }

int StreamHeapFreeNodes()
{
    const BYTE* heap = *reinterpret_cast<const BYTE* const*>(0x0088EB00 + 0x6A4);
    if (!heap) return -1;
    const BYTE* sentinel = heap + 0x0C;
    int n = 0;
    __try {   // menu only; the list may change under us on a loader frame
        for (const BYTE* p = *reinterpret_cast<const BYTE* const*>(sentinel); p && p != sentinel && n < 100000; p = *reinterpret_cast<const BYTE* const*>(p)) ++n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return n;
}
