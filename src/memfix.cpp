// Crash fixes for the game's own memory and file code (vanilla bugs, measured and fixed in NFSU2_ShaderLoader
// 2026-09-30 .. 2026-10-07; ported here so vanilla and RTX Remix players get them without the ShaderLoader).
//
// 1. Heap guard (bFree 0x440540, EA: UnloadMemoryFile): random crash when a track unloads - the block's list
//    neighbour is already gone and the unlink (0x4405CF) writes into it. Every free probes both neighbours first;
//    a dead one is logged and pointed at a spare header of ours, so the unlink writes there.
// 2. Pool lock: bWareMalloc 0x440390 links the new block into the pool's in-use list AFTER the pool mutex is
//    released, bFree unlinks BEFORE taking it - two threads in one pool corrupt it (multi-core crashes; WidescreenFix's
//    single-core workaround hangs new systems). One recursive lock of ours around both (taken before the pool mutex,
//    never the other way round). Allocations / frees from other threads are counted for the menu.
// 3. File guard: RealSystem file system (vtable 0x7C2E2C) slot 3 open 0x6F69E6 takes a request node from a free
//    list without a check (0x6F6B56) - after quick shop visits the list is empty and 0x6F6AF9 writes to NULL. Open
//    and release (slot 4 0x6F69B2) go through one lock; an empty list waits up to 200 ms, then answers -1
//    (the caller treats it as "cannot open" and tries the next path).
// 4. Low memory for D3DX (LAA only): the game's static D3DX (2004) tells a name from a D3DXHANDLE by the sign bit
//    (0x65BE31 cmp esi,0 / jle) - a name above 0x80000000 is taken for a handle and crashes (0x65BED6). operator new
//    (thunk 0x575620 -> malloc 0x75C6C8) called from D3DX code 0x63C000..0x6E0000 is served from a heap of ours whose
//    whole reserve lies below 2 GB; free 0x75C6DA gives such blocks back to it. D3DX's private arena for the effect
//    text compiler (allocator 0x671CCB, `call VirtualAlloc` MEM_RESERVE at 0x671D1D) is reserved below 2 GB as well -
//    the game compiles its effects from text resources at load (docs LAA_AUDIT.md).
#include "core.h"
#include <cstdio>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)

namespace {
MemFixStats g_stats = { 0, 0, 0, 0, 0, 0x7FFFFFFF };
DWORD g_mainTid = 0;
bool  g_lockOn = false;
CRITICAL_SECTION g_poolCs;

// ---- 1 + 2: bFree / bWareMalloc --------------------------------------------------------------
typedef void  (__cdecl* BFree_t)(void* p);
typedef void* (__cdecl* WareMalloc_t)(int size, const char* name, int line, int flags);
BFree_t g_origFree = nullptr;
WareMalloc_t g_origMalloc = nullptr;
DWORD g_spare[8] = {};
bool  g_guardOn = false;
struct GuardHit { void* block; DWORD next, prev; void* caller; int bad; };
GuardHit g_hits[8]; LONG g_hitsLogged = 0;   // written on any thread, logged from the frame

int ProbeBlock(const BYTE* h)   // 0 fine / no list block, bit 0 prev dead, bit 1 next dead, 4 header unreadable
{
#define AV_ONLY (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    __try { if (h[9] != 0x22) return 0; } __except (AV_ONLY) { return 4; }
    int bad = 0;
    __try { volatile DWORD v = **reinterpret_cast<const DWORD* const*>(h + 4); (void)v; } __except (AV_ONLY) { bad |= 1; }
    __try { volatile DWORD v = *(*reinterpret_cast<const DWORD* const*>(h) + 1); (void)v; } __except (AV_ONLY) { bad |= 2; }
    return bad;
}

void GuardBlock(void* p, void* caller)
{
    // blocks of the fixed pools (table 0x82802C..0x82804C: base +4, size +8) carry no list header
    for (const DWORD* slot = reinterpret_cast<const DWORD*>(0x0082802C); slot < reinterpret_cast<const DWORD*>(0x0082804C); ++slot) {
        const DWORD* pool = reinterpret_cast<const DWORD*>(*slot);
        if (pool && reinterpret_cast<DWORD>(p) >= pool[1] && reinterpret_cast<DWORD>(p) - pool[1] < pool[2]) return;
    }
    BYTE* h = static_cast<BYTE*>(p) - 0x14;
    int bad = ProbeBlock(h);
    if (!(bad & 3)) return;
    DWORD* link = reinterpret_cast<DWORD*>(h);
    LONG k = InterlockedIncrement(&g_stats.heapGuardHits) - 1;
    if (k < 8) { g_hits[k].block = p; g_hits[k].next = link[0]; g_hits[k].prev = link[1]; g_hits[k].caller = caller; g_hits[k].bad = bad; }
    if (bad & 1) link[1] = reinterpret_cast<DWORD>(g_spare);   // the unlink is [prev] = next ; [next + 4] = prev
    if (bad & 2) link[0] = reinterpret_cast<DWORD>(g_spare);
}

void __cdecl FreeHook(void* p)
{
    void* caller = _ReturnAddress();
    const bool locked = g_lockOn;   // latched: enter and leave must agree even if the flag changes meanwhile
    if (locked) {
        EnterCriticalSection(&g_poolCs);
        if (g_mainTid && GetCurrentThreadId() != g_mainTid && p) InterlockedIncrement(&g_stats.otherThreadFrees);
    }
    if (p && g_guardOn) GuardBlock(p, caller);
    g_origFree(p);
    if (locked) LeaveCriticalSection(&g_poolCs);
}

void* __cdecl MallocHook(int size, const char* name, int line, int flags)
{
    EnterCriticalSection(&g_poolCs);
    if (g_mainTid && GetCurrentThreadId() != g_mainTid) InterlockedIncrement(&g_stats.otherThreadAllocs);
    void* r = g_origMalloc(size, name, line, flags);
    LeaveCriticalSection(&g_poolCs);
    return r;
}

// ---- 3: file requests -------------------------------------------------------------------------
typedef int  (__fastcall* Open_t)(BYTE* self, void* edx, const char* path, int mode, int* out);
typedef void (__fastcall* Release_t)(BYTE* self, void* edx, int index);
const Open_t    kOpen    = reinterpret_cast<Open_t>(0x006F69E6);
const Release_t kRelease = reinterpret_cast<Release_t>(0x006F69B2);
CRITICAL_SECTION g_fileCs;

bool FreeEmpty(const BYTE* self) { return *reinterpret_cast<const DWORD*>(self + 0x3C) == 0 || *reinterpret_cast<const int*>(self + 0x44) <= 0; }

int __fastcall OpenHook(BYTE* self, void* edx, const char* path, int mode, int* out)
{
    EnterCriticalSection(&g_fileCs);
    if (FreeEmpty(self)) {
        InterlockedIncrement(&g_stats.fileWaits);
        for (int i = 0; i < 200 && FreeEmpty(self); ++i) { LeaveCriticalSection(&g_fileCs); Sleep(1); EnterCriticalSection(&g_fileCs); }
        if (FreeEmpty(self)) {
            InterlockedIncrement(&g_stats.fileRefused);   // reported from the frame
            LeaveCriticalSection(&g_fileCs);
            return -1;
        }
    }
    int f = *reinterpret_cast<const int*>(self + 0x44);
    if (f < g_stats.fileMinFree) g_stats.fileMinFree = f;
    int r = kOpen(self, edx, path, mode, out);
    LeaveCriticalSection(&g_fileCs);
    return r;
}

void __fastcall ReleaseHook(BYTE* self, void* edx, int index)
{
    EnterCriticalSection(&g_fileCs);
    kRelease(self, edx, index);
    LeaveCriticalSection(&g_fileCs);
}

// ---- 4: D3DX below 2 GB -----------------------------------------------------------------------
HANDLE g_lowHeap = nullptr;
volatile LONG g_lowMisses = 0;
DWORD  g_lowBase = 0, g_lowEnd = 0;
constexpr DWORD kD3dxLo = 0x0063C000, kD3dxHi = 0x006E0000, kMalloc = 0x0075C6C8;
typedef void* (__cdecl* Malloc_t)(size_t);
typedef void  (__cdecl* CrtFree_t)(void*);
CrtFree_t g_origCrtFree = nullptr;

void* __cdecl NewHook(size_t n)
{
    DWORD ra = reinterpret_cast<DWORD>(_ReturnAddress());
    if (ra >= kD3dxLo && ra < kD3dxHi && g_lowHeap)
        if (void* p = HeapAlloc(g_lowHeap, 0, n ? n : 1)) return p;
        else InterlockedIncrement(&g_lowMisses);
    return reinterpret_cast<Malloc_t>(kMalloc)(n);
}

void __cdecl CrtFreeHook(void* p)
{
    DWORD a = reinterpret_cast<DWORD>(p);
    if (a >= g_lowBase && a < g_lowEnd) { HeapFree(g_lowHeap, 0, p); return; }
    g_origCrtFree(p);
}

// stdcall like VirtualAlloc: the arena reservation (lpAddress 0, MEM_RESERVE) is placed below 2 GB
LPVOID WINAPI LowReserve(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    if (!addr && type == MEM_RESERVE) {
        const DWORD want = static_cast<DWORD>((size + 0xFFFF) & ~static_cast<SIZE_T>(0xFFFF));
        MEMORY_BASIC_INFORMATION mbi;
        for (DWORD a = 0x10000; a < 0x80000000u; ) {
            if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) break;
            const DWORD base = reinterpret_cast<DWORD>(mbi.BaseAddress), end = base + static_cast<DWORD>(mbi.RegionSize);
            if (mbi.State == MEM_FREE) {
                const DWORD b = (base + 0xFFFF) & ~0xFFFFu;
                if (b < end && end - b >= want && b + want <= 0x80000000u)
                    if (LPVOID p = VirtualAlloc(reinterpret_cast<void*>(b), size, type, protect)) return p;
            }
            if (end <= a) break;
            a = end;
        }
    }
    return VirtualAlloc(addr, size, type, protect);
}

void LowMemInstall()
{
    g_lowHeap = HeapCreate(0, 1u << 20, 128u << 20);   // fixed size: one reservation, made now while the low space is free
    if (!g_lowHeap) { Log("[FAIL] low heap: HeapCreate"); return; }
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(g_lowHeap, &mbi, sizeof(mbi));
    g_lowBase = reinterpret_cast<DWORD>(mbi.AllocationBase);
    DWORD a = g_lowBase;
    while (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) && reinterpret_cast<DWORD>(mbi.AllocationBase) == g_lowBase) a += static_cast<DWORD>(mbi.RegionSize);
    g_lowEnd = a;
    if (g_lowEnd > 0x80000000u) { Log("[SKIP] low heap is not below 2 GB (%08X..%08X)", g_lowBase, g_lowEnd); HeapDestroy(g_lowHeap); g_lowHeap = nullptr; g_lowBase = g_lowEnd = 0; return; }
    static const BYTE freeOrig[5] = { 0x6A, 0x0C, 0x68, 0xF8, 0x39 };
    int hf = PatchAddHook("D3DX below 2 GB: free", "memfix", 0x0075C6DA, freeOrig, 5, reinterpret_cast<void*>(&CrtFreeHook), reinterpret_cast<void**>(&g_origCrtFree), "CRT free gives low-heap blocks back");
    if (hf < 0 || !PatchIsOn(hf)) { Log("[SKIP] D3DX low memory: free not hooked"); return; }
    const DWORD relOrig = kMalloc - (0x00575620 + 5), relNew = reinterpret_cast<DWORD>(&NewHook) - (0x00575620 + 5);
    int hn = PatchAdd("D3DX below 2 GB: operator new", "memfix", 0x00575621, &relOrig, &relNew, 4, "thunk 0x575620: D3DX callers get low memory");
    if (!PatchSet(hn, true)) { Log("[SKIP] D3DX low memory: operator new thunk not original"); return; }
    Log("[OK] D3DX allocations below 2 GB (%08X..%08X, %u MB)", g_lowBase, g_lowEnd, (g_lowEnd - g_lowBase) >> 20);
    {
        static const BYTE orig[6] = { 0xFF, 0x15, 0xF4, 0x31, 0x78, 0x00 };   // call [VirtualAlloc]
        BYTE repl[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        *reinterpret_cast<DWORD*>(repl + 1) = reinterpret_cast<DWORD>(&LowReserve) - (0x00671D1D + 5);
        int h = PatchAdd("D3DX below 2 GB: compiler arena", "memfix", 0x00671D1D, orig, repl, 6, "VirtualAlloc reserve of D3DX's private arena");
        PatchSet(h, true);
    }
}

// 5. eReleaseRenderTargets 0x5BA610 releases the back-buffer surface 0x87097C without a null check (every other
//    target there is checked). After a failed device Reset the game skips eCreateRenderTargets and calls the release
//    again on the next attempt -> read of 0 at 0x5BA6C7 (options menu crash, 2026-10-17).
DWORD g_ptrReleaseResume = 0x005BA6CD;
__declspec(naked) void ReleaseBackBufferStub()
{
    __asm {
        mov  eax, dword ptr ds:[0x0087097C]
        test eax, eax
        jz   skip
        mov  edx, [eax]
        push eax
        call dword ptr [edx + 8]
    skip:
        jmp  dword ptr [g_ptrReleaseResume]
    }
}

bool ExeIsLaa()
{
    const BYTE* img = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(img + reinterpret_cast<const IMAGE_DOS_HEADER*>(img)->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}
} // namespace

void MemFixInstall()
{
    {
        static const BYTE orig[11] = { 0xA1, 0x7C, 0x09, 0x87, 0x00, 0x8B, 0x10, 0x50, 0xFF, 0x52, 0x08 };
        int h = PatchAddJmp("back buffer release null check", "memfix", 0x005BA6C2, orig, 11, reinterpret_cast<const void*>(&ReleaseBackBufferStub),
                            "eReleaseRenderTargets: 0x87097C released only when it exists");
        PatchSet(h, true);
    }
    if (g_cfg.lowMemD3dx && ExeIsLaa()) LowMemInstall();
    InitializeCriticalSectionAndSpinCount(&g_poolCs, 4000);
    if (g_cfg.heapGuard || g_cfg.poolLock) {
        g_spare[0] = g_spare[1] = reinterpret_cast<DWORD>(g_spare);
        g_guardOn = g_cfg.heapGuard != 0;
        static const BYTE freeOrig[7] = { 0x57, 0x8B, 0x7C, 0x24, 0x08, 0x85, 0xFF };
        static const BYTE mallocOrig[8] = { 0x51, 0x53, 0x55, 0x57, 0x8B, 0x7C, 0x24, 0x18 };
        // the lock needs both sides: malloc first (it always locks), then the flag, then free - no free can see a
        // half-installed lock
        bool mallocOn = false;
        if (g_cfg.poolLock) {
            int hm = PatchAddHook("bWareMalloc pool lock", "memfix", 0x00440390, mallocOrig, 8, reinterpret_cast<void*>(&MallocHook), reinterpret_cast<void**>(&g_origMalloc), "allocation + list link under one lock");
            mallocOn = hm >= 0 && PatchIsOn(hm);
        }
        g_lockOn = mallocOn;
        int hf = PatchAddHook("bFree heap guard / pool lock", "memfix", 0x00440540, freeOrig, 7, reinterpret_cast<void*>(&FreeHook), reinterpret_cast<void**>(&g_origFree), "dead neighbours redirected; pool lock");
        if (hf < 0 || !PatchIsOn(hf)) { g_lockOn = false; g_guardOn = false; }
        Log("memfix: heap guard %s, pool lock %s", g_guardOn && PatchIsOn(hf) ? "on" : "off", g_lockOn ? "on" : "off");
    }
    if (g_cfg.fileGuard) {
        InitializeCriticalSectionAndSpinCount(&g_fileCs, 4000);
        const DWORD openOrig = 0x006F69E6, relOrig = 0x006F69B2;
        const DWORD openNew = reinterpret_cast<DWORD>(&OpenHook), relNew = reinterpret_cast<DWORD>(&ReleaseHook);
        int h0 = PatchAdd("file guard: open", "memfix", 0x007C2E2C + 3 * 4, &openOrig, &openNew, 4, "RealSystem file system vtable slot 3");
        int h1 = PatchAdd("file guard: release", "memfix", 0x007C2E2C + 4 * 4, &relOrig, &relNew, 4, "RealSystem file system vtable slot 4");
        if (PatchAt(h0) && PatchAt(h1) && PatchAt(h0)->state == PS_Off && PatchAt(h1)->state == PS_Off) { PatchSet(h0, true); PatchSet(h1, true); }
        else Log("[SKIP] file guard: vtable slots are not the game's");
    }
}

void MemFixFrame()
{
    if (!g_mainTid) g_mainTid = GetCurrentThreadId();
    LONG hits = g_stats.heapGuardHits;
    while (g_hitsLogged < hits && g_hitsLogged < 8) {
        const GuardHit& h = g_hits[g_hitsLogged++];
        Log("[heap guard] block %p: dead %s%s neighbour (next %08X prev %08X), freed from %p - unlink redirected, no crash",
            h.block, (h.bad & 1) ? "prev" : "", (h.bad & 2) ? ((h.bad & 1) ? "+next" : "next") : "", h.next, h.prev, h.caller);
    }
    static LONG refusedLogged = 0, lowLogged = 0;
    if (g_stats.fileRefused != refusedLogged) { refusedLogged = g_stats.fileRefused; Log("[file guard] %ld file requests answered 'cannot open' instead of the crash 0x6F6AF9", refusedLogged); }
    if (g_lowMisses != lowLogged) { lowLogged = g_lowMisses; Log("[WARN] %ld D3DX allocations not served from the low heap (too big or full)", lowLogged); }
}
MemFixStats MemFixGet() { return g_stats; }
