# LOD A everywhere

Target: SPEED2.EXE v1.2 (32-bit, image base 0x400000).

RTX Remix identifies geometry by hashing the meshes it is given. A LOD switch hands it a different mesh, so
replacements and lights attached to an object vanish or flicker whenever the game changes LOD. ForceLOD 2 therefore
draws exactly one mesh per object, every frame, in every view: LOD A. The same is true for cars.

Code: `src/lod.cpp`, `src/loda_units.h` (reference data, not used at run time).

## Data layout

| Structure | Stride | Fields used |
|---|---|---|
| SceneryInfo | 0x44 | +0x20 LOD hashes A/B/C, +0x30 / +0x34 / +0x38 eModel* A/B/C, +0x3C radius |
| SceneryInstance | 0x40 | +0x18 info index, +0x1A flags |
| eModel | 0x18 | +0x0C eSolid* (0 = unbound) |
| Cull info | 0x90 | +0x44 pass flags (0x1800 = reflection / env views), +0x4C list cursor, +0x50 list end |

In many infos A and B are the same solid (same hash, the eModel is reused): 6 394 of 12 858 infos in L4RA, 6 033 in
L4RD. Forcing A is a no-op there.

## The world LOD hook (0x5D6653)

`ScenerySection_AddInstance` 0x5D6570 picks the LOD in the block 0x5D6653 .. 0x5D673A. Vanilla:

- `px <= 1`: culled before this block (0x5D6646).
- Reflection / env views (flags 0x1800): `px < 32` culled, otherwise B or C.
- View 3 / split screen: B.
- Main views: `px < 17` culled; A only if it is bound and the triangle count, score and World Detail setting allow
  it; otherwise B.
- View flag 0x20 (`px < 16` culled, then B) and view mode >= 3 (`px < 23` culled, then B).

The plugin writes a 6-byte JMP over `mov ebx,[ebx+44h]; test bh,18h` at 0x5D6653 and replaces the whole block.
Registers at the hook: `eax` = pixel size, `ebx` = cull info, `edi` = SceneryInfo, `esi` = SceneryInstance; the x87
stack is empty. The hook saves everything with `pushad`, calls the decision function, restores, and leaves through
one of three exits:

| Exit | Condition |
|---|---|
| 0x5D673A | `ebx` = chosen eModel*; the game writes the visibility entry |
| 0x5D69A1 | culled (function epilogue, same stack as the vanilla cull branch) |
| 0x5D671A | the game's own reflection branch (needs `eax` = px), used only when `ReflectionLodA` is off |

### Decision

1. Reflection / env view and `ReflectionLodA` off: the game's branch (B/C, as vanilla).
2. Pixel limit: `WorldMinPixelSize` (default 4, vanilla 17); reflection views at least 32; extra sections of the full
   map may use `ExtraMinPixelSize`. Capture mode and the guard radius skip the limit for player views.
3. Hysteresis: an instance inside the band [limit, limit + `WorldPixelHysteresis`) keeps its last decision, so objects
   do not flicker at the edge. The state is a direct-mapped table of 8192 entries keyed by instance pointer, with
   separate tables for player views and reflection views (their limits differ).
4. LOD A present and bound: draw A.
5. LOD A present but unbound: bind it on the spot (below). If that succeeds, draw A in the same frame.
6. LOD A present but its solid is not in memory: **cull**. Never B.
7. No LOD A in the data at all: the only mesh there is (B, else C). That is always the same mesh, so it is just as
   stable.

### Binding on the spot

The binding follows the game's loader exactly: `eModel::Init` 0x493600 -> `eFindSolid(hash, 0)` 0x48DF90 ->
`eModel::SetSolid` 0x48D780. `SetSolid` unlinks the model from the unbound list 0x82DBC0 and links it into the solid's
model list (solid +0x88 / +0x8C). The lists it touches are iterated only by the chunk loaders, the unload paths and
the header add/remove functions, never by the cull-tree walk or the draw path, which read only model+0x0C. So binding
inside the culler does not break an iteration in progress.

- No binding while `[0x864F60] != 0`. This is not a lock: it is the pointer delta used while loaded chunks are being
  relocated (models are parked during that time). The game's loader does not bind either.
- Hashes 0, 0xBE43EDBB (SKYDOME) and 0x72923C81 are the loader's "no model" values and are skipped.
- Negative cache: a hash whose solid is not in memory is looked up again only after 30 frames (4096-entry table).
  `eFindSolid` walks every pack header when its 0x2000-bucket table has a collision, and the culler would otherwise
  repeat that for every instance, view and frame.

In vanilla a model whose solid is in memory is practically never unbound: `eModel::Init` binds at creation, and every
solid-pack load calls `eModel_RebindUnboundModels` 0x490650 for all unbound models. Binding in the culler is a safety
net for edge cases. The real guarantee that LOD A is in memory comes from the streamer (next section).

### Why never fall back to B

A fallback would draw B in one frame and A in the next as soon as the pack arrives: two different meshes, two
different Remix hashes, and the replacement or light flickers. A culled object for a frame is the lesser evil, and
with the LOD A units always loaded it should not happen at all. The menu and the log count "A missing -> culled"
so it can be checked; outside the cases below the counter must stay at 0 after a track has loaded.

## Which LOD A units are kept loaded, and why

The game splits the solids of a region into units. Analysis of all regions (static, from the track files):

- Every SceneryInfo has its LOD A solid either in the **unit of its own section** or in an **X unit below 50**. It is
  never only in another scenery unit and never only in the split-screen set (X50+).
- In L4RA, 85 % of all placed objects (by instance) have their LOD A in an X unit. X00 carries most of them (6 948 of
  7 094 such infos in L4RA). X01..X25 are **area packs**: only the zones around their users load them (in L4RA,
  X08 / X09 / X10 belong to the C and D zones).
- X units hold only solids; the matching Y units hold only textures (X02 <-> Y02 and so on, wanted in the same zones).
  LOD A often uses textures that B does not, so X and Y must always be wanted in pairs; otherwise LOD A would be drawn
  with the default texture.

How vanilla wants them: `BuildWantedList` 0x5E5110 adds a fixed list (Y00 = 2500, X00 = 2400, Z00 = 2600, plus the
track's Z list and a few extras) in every zone. The area packs X01..X25 / Y01..Y25 come only through the zone lists,
i.e. they follow the zone of the **car**, not the camera.

Vanilla is self-consistent: in every load set of every region, each loaded section has the X unit of its LOD A in the
same set. LOD A is missing only when something draws sections outside the vanilla list: the full map, the guard
radius, a camera far from the car. Example in L4RA: car in zone A01, a C07 section drawn by the plugin needs
X02 / X08 / X09 / X10, but A01 wants only X01.

**The fix.** After the game's `BuildWantedList` (hook on the call at 0x5E7BEC), every X and Y unit with a number
below 50 that exists in the loaded region's unit table (numbers 24xx and 25xx with `n % 100 < 50`) is added through
`AddSectionsToLoad` 0x5DACB0 for both slots. The units are read at run time, so mod tracks with other unit numbers are
covered too. `SwitchZones` never unloads them again, because they are always wanted.

Extra memory on top of vanilla (unit size = unit +0x18, exactly what the streamer allocates):

| Region | X/Y < 50 units | Extra beyond vanilla's fixed list |
|---|---|---|
| L4RA | 16 | 3 463 096 B (3.3 MB) |
| L4RD | 24 | 6 351 864 B (6.1 MB) |
| L4RG | 3 | 44 448 B (Y01) |
| L4RB, L4RC, L4RF | 2 (X00, Y00) | 0 |

The largest vanilla load set including the fixed list is about 40 MB in L4RA; with the additions it is still well
inside the vanilla streaming pool. With the full map on, every unit except X/Y 50+ is wanted anyway, which includes
these.

Not done in split screen: the game draws B there and `AddSectionsToLoad` remaps X/Y numbers below 50 to 50+.

Known gap in the data: in L4RB sections B02/B03, `XB_BridgeSupportC_1a_RB_00` has neither its A nor its B solid in any
file. Vanilla never draws those two instances either.

## Cars

Cars do not go through the scenery culler. The plugin uses the engine's own debug overrides and a few constants:

| Site | Vanilla | Plugin default | Meaning |
|---|---|---|---|
| 0x802728 (int) | -1 | 0 | `ForceCarLOD`: body LOD, -1 = automatic, 0 = A. Read after the game's own choice in Render 0x6211EC and RenderFast 0x616795 |
| 0x80272C (int) | -1 | 0 | `ForceTireLOD`: tyre LOD, same readers |
| 0x60C5A9 (imm8) | 0 | 1 | `mov [esp+13h],0` at 0x60C5A5 in `CarPartCuller::CullParts`: the only place that hides a part (underbody, brakes, sides) by view angle. 1 = always visible |
| 0x620CB9, 0x616343, 0x6156E1 (imm8) | 4 | 1 | `cmp edi/esi,4; jl` in Render, RenderFast and RenderFlaresOnCar: cars below N pixels are not drawn |
| 0x7A66FC (float) | 18.0 | 0 | low-detail limit (read at 0x6166E7 / 0x621129): at or below N pixels, wheels, brakes and attachments are dropped. 0 = never |
| 0x60D544 | `74 1E` | `EB 1E` | `ShouldCarBeRendered`: a respawning car is hidden every second frame (blinking). With `jmp` it returns "poofed" (0), so it is always drawn; callers do `test eax,eax; jnz skip` (0x6317AC, 0x61B22E) |

All of these are live and switchable from the menu.
