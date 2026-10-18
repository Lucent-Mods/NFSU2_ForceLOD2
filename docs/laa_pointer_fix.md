# Large-address-aware SPEED2.EXE: pointer compares and other crash fixes

Target: SPEED2.EXE v1.2 (32-bit, image base 0x400000). File offset = VA - 0x400000 for `.text`, `.rdata` and `.data`.

Code: `src/pools.cpp` (pointer compares), `src/memfix.cpp` (D3DX below 2 GB and the other crash fixes).

## Why unsigned compares are needed

A 32-bit process normally gets 2 GB of address space, so every pointer is below 0x80000000. With the
`LARGE_ADDRESS_AWARE` flag set in the PE header (Characteristics bit 0x0020), Windows gives it up to 4 GB, and
pointers at or above 0x80000000 appear. RTX Remix, big streaming pools and the full map need that space.

The game's code was compiled for 2 GB. Where it compares two pointers with a signed branch (`jl`, `jge`, ...), a pointer
above 2 GB looks negative and the comparison flips. The fix is to replace the signed branch with its unsigned
counterpart (`jl` -> `jb`, `jge` -> `jae`, `jnl` -> `jnb`), which is correct for every address.

The plugin applies these patches only when the running SPEED2.EXE actually has the LAA flag (and `LaaFix` is on).
Without the flag the patches are not needed.

## How the sites were found

Statically, from the disassembly of SPEED2.EXE:

1. **Full manual reading** of every conditional branch in the memory code: the bMemory pools (`MemoryPool_Free`
   0x43FA50, `AllocateMemory` 0x43FC80, `bWareMalloc` 0x440390, `bFree` 0x440540, ...), the slot pools, the stream
   heap (0x5DA370 .. 0x5DA87C, constructor, destructor, `InitMemoryPool`, override callbacks), the car loader pool, the
   front-end multipool, FrameMalloc, the file / resource loaders and the scenery draw list.
2. **Linear sweep of the whole `.text`** (1 242 564 instructions) with several heuristics:
   - a signed condition after `cmp`/`test` whose operand is dereferenced nearby as a base register
     (235 hits; about 210 are `cmp reg, offset StaticEnd; jl` over static arrays inside the image, which is always
     below 2 GB; the rest were checked one by one);
   - range checks: two signed compares on the same register within 14 instructions, the second bound computed with
     `add`/`lea` (392 pairs, 114 after filtering, all checked; finds both known pairs again);
   - allocator results tested with a signed branch (`test eax,eax; jle/js` after any of 2 365 allocator calls): 0 hits;
   - pointer-difference comparators (`sub` + `ret` with stack arguments, all 18 `bList::Sort` callers): only unsigned
     or integer fields;
   - signed compares on 27 globals that are known to hold allocator results: 0 hits.

`sar` / `idiv` on the difference of two pointers into the **same** allocation stay correct above 2 GB (the difference
is below 2 GB). No site evaluates the difference between pointers from **different** allocations with a signed
operation.

## Required sites (pointer vs pointer from different allocations)

| VA | Original | Patched | Function | What goes wrong |
|---|---|---|---|---|
| 0x44055E | `7C 09` | `72 09` | `bFree` (EA name `UnloadMemoryFile`), pool override range, lower bound | |
| 0x440567 | `7C 44` | `72 44` | `bFree`, pool override range, upper bound | a stream-heap block is not recognised, falls into the bMemory path, the header check `[edi+9] == 0x22` fails -> `int3` at 0x4405A9 |
| 0x5DA4E3 | `7C 06` | `72 06` | `StreamHeap_Alloc` 0x5DA4A0, hint >= block start | |
| 0x5DA4E9 | `7C 4E` | `72 4E` | `StreamHeap_Alloc`, hint < block end | no block found for the wanted address -> returns 0 -> loading hang |

`bFree` compares the freed pointer against the start (`[eax+4]`) and start + size (`[eax+8]`) of each override heap in
the table 0x82802C .. 0x82804C. `start + size` cannot overflow below the WoW64 limit 0xFFFE0000. The loop compare over
the static table itself stays signed; both sides are inside the image.

`StreamHeap_Alloc` compares a wanted address (from `TrackStreamer_ReserveUnits` 0x5DADB0: slot << 7 + reserved base)
against each block node (+0x08 address, +0x0C size). All other signed compares in that function compare sizes.

These four sites are also patched, with the same bytes, by other community fixes for this game. The patch register
detects identical bytes written by another plugin and leaves them alone.

## Single-buffer loops (also patched)

Signed `p < end` with `p` and `end` inside the same buffer is wrong only if that buffer **spans** 0x80000000. That is
rare, but possible for a `VirtualAlloc` or heap block. The plugin patches these six as well:

| VA | Original | Patched | Function |
|---|---|---|---|
| 0x719C5C | `7C B4` | `72 B4` | VP6 movie line scaler |
| 0x719D30 | `7C A8` | `72 A8` | VP6 movie line scaler (MMX) |
| 0x76EC86 | `0F 8D` | `0F 83` | D3DX MMX box filter |
| 0x76ECA2 | `7D 71` | `73 71` | D3DX MMX box filter |
| 0x76ED13 | `7C 8F` | `72 8F` | D3DX MMX box filter, inner loop |
| 0x76ED27 | `0F 8C` | `0F 82` | D3DX MMX box filter, row loop |

## Checked and clean

Unsigned or equality only: `MemoryPool_Free` (`ja` 0x43FA8A), the pool comparator 0x43CAE0 (`cmp; sbb; inc`),
`StreamHeap_Free` and the heap constructor / destructor, `FrameMalloc` / `FrameMallocMatrices` (`jb`), the car loader
pool and front-end multipool "contains" tests, the scenery draw list cursor tests (0x5D6751, 0x5D67BB, 0x5D6D4C: `jae`),
the stream pack loader comparator 0x488D30, `TrackStreamer_ReserveUnits` 0x5DAED8.

Signed but not pointers: block sizes, alignment remainders, allocation counters, slot indices (the pointer difference
is divided unsigned, only the quotient is compared signed), section IDs, file positions, and loop counters.

## D3DX below 2 GB

The game links a 2004 D3DX statically. That D3DX tells a parameter **name** (a `char*`) from a **D3DXHANDLE** by the
sign bit: `cmp esi,0` / `jle` at 0x65BE2F / 0x65BE31, then `not esi` (handle = ~pointer). A name or internal object above
0x80000000 is taken for a handle and crashes (0x65BED6). The same idiom appears at 26 decoder sites (0x650F4E ..
0x66F505), and over a hundred other `not` sites create handles from pointers. Patching branches cannot fix this; the
memory itself has to stay below 2 GB.

The fix (only with an LAA exe, `LowMemD3dx`):

1. At plugin start, before anything else is allocated, a private Win32 heap is created
   (`HeapCreate(0, 1 MB, 128 MB)`, fixed size so the whole reservation is made at once). If it does not lie entirely
   below 2 GB, the fix is skipped.
2. `operator new` is a thunk at 0x575620 (`jmp` to `malloc` 0x75C6C8). Its rel32 at 0x575621 is redirected. Callers
   whose return address lies in the D3DX code range 0x63C000 .. 0x6E0000 get memory from the low heap; everyone else
   gets the CRT `malloc` as before. That range covers every D3DX call of `operator new` (339 sites); it also catches
   14 calls from game code after 0x6D5200, which only costs low-heap space.
3. CRT `free` 0x75C6DA is hooked: blocks inside the low heap go back to it, all others to the CRT. D3DX also calls
   `free` directly once (0x645F3F); the hook covers it.
4. D3DX's private arena allocator 0x671CCB (used by the effect text compiler) reserves memory with
   `VirtualAlloc(NULL, n, MEM_RESERVE)` at 0x671D1D. That call is replaced by a function that searches the address
   space below 2 GB for a free, 64 KB aligned region of the requested size and reserves it there; if none is found,
   the normal `VirtualAlloc` runs. The game compiles its effects from text resources at load, so this path is used.
5. A fixed-size Win32 heap refuses blocks larger than 0x7FFF8 bytes. Such allocations fall back to the CRT and are
   counted and logged as "not served from the low heap".

D3DX also has a `MapViewOfFile` path (0x65A9E9) for file-based creation. The game creates its effects from resources
in the image (`CreateEffectFromResource`, call at 0x5D1F56), so that path is not used.

## Other crash fixes in memfix.cpp

These fix bugs of the unmodified game. They were first developed for another project of the author and ported here so
that vanilla and RTX Remix players get them too. All are on by default and can be switched off in the INI.

### Heap guard (`HeapGuard`)

Random crash when a track unloads: in `bFree` 0x440540 the block's list neighbour is already gone, and the unlink at
0x4405CF writes into it. Block header: +0 next, +4 prev, +8 pool, +9 magic 0x22 (data starts 0x14 after the header).

`bFree` is hooked. Before every free, both neighbours are probed (an access violation while reading them is caught).
A dead neighbour is logged and replaced by a spare header of the plugin, so the unlink writes there instead. Blocks of
the fixed pools (table 0x82802C .. 0x82804C, base +4, size +8) carry no list header and are skipped.

### Pool lock (`PoolLock`)

`bWareMalloc` 0x440390 links the new block into the pool's in-use list **after** releasing the pool mutex, and `bFree`
unlinks **before** taking it. Two threads in one pool corrupt it: crashes on multi-core systems. (Forcing the game onto
one core avoids it, but hangs on new systems.)

Both functions are hooked and wrapped in one recursive lock of the plugin, always taken before the pool mutex and never
the other way round. The override callbacks (stream heap only) do not wait on anything, so there is no lock inversion.
The malloc hook is enabled first and the free hook decides once per call whether it locked, so a free can never leave
a lock it did not enter. Allocations and frees from threads other than the main thread are counted for the menu.

### File guard (`FileGuard`)

The RealSystem file system (vtable 0x7C2E2C) takes a request node from a free list without checking it in slot 3, the
open function 0x6F69E6 (at 0x6F6B56). After quick shop visits the list is empty and 0x6F6AF9 writes to NULL.

Slots 3 (open, 0x6F69E6) and 4 (release, 0x6F69B2) are replaced in the vtable, and both run under one lock of the
plugin. When the free list (head +0x3C, count +0x44) is empty, open waits up to 200 ms for a release; if it is still
empty, it returns -1. The caller treats that as "cannot open" and tries its next path.

### Back buffer release

`eReleaseRenderTargets` 0x5BA610 releases the back-buffer surface 0x87097C without a null check (every other target
there is checked). After a failed device `Reset` the game skips `eCreateRenderTargets` and calls the release again on
the next attempt, which reads address 0 at 0x5BA6C7 (crash in the options menu). The 11 bytes at 0x5BA6C2 are replaced
with a JMP to a stub that releases the surface only when it exists, then resumes at 0x5BA6CD.
