# Patch and hook sites

Every byte the plugin writes into SPEED2.EXE v1.2 goes through the patch register (`src/patch.cpp`):

- `PatchAdd`: replace `len` bytes at an address (immediates, operands, branch opcodes).
- `PatchAddJmp`: a 5-byte JMP to plugin code, padded with NOPs to `len`.
- `PatchAddHook`: an inline hook through MinHook at a function entry.

A site is written only if it still holds the original bytes. If another plugin already wrote the identical bytes, the
site is marked "shared" and never reverted; any other content is "foreign" and left alone. The menu lists every site
with its state.

Addresses below are the ones passed to the register. For operand patches the instruction address is given in
parentheses. "Startup" sites are set once; "live" sites follow the menu.

## lod (`src/lod.cpp`)

| Address | Kind | Bytes / target | What it does |
|---|---|---|---|
| 0x5D6653 | JMP, 6 B | `8B 5B 44 F6 C7 18` | World LOD choice in `ScenerySection_AddInstance`: always LOD A, unbound A bound or culled, own pixel limit. Exits 0x5D673A / 0x5D69A1 / 0x5D671A |
| 0x5E7BEC | call, 5 B | `E8 1F D5 FF FF` (`call BuildWantedList` 0x5E5110) | After the game's wanted list: X/Y units < 50 always wanted; with the full map every unit except X/Y 50+ |
| 0x5E9B5A | 2 B | `74 02` -> `EB 02` | `BuildCullLists`: precull never switched off by the BooBoo map cells (live) |
| 0x802728 | int | -1 -> 0 | `ForceCarLOD` body override (live) |
| 0x80272C | int | -1 -> 0 | `ForceTireLOD` tyre override (live) |
| 0x60C5A9 | imm8 | 0 -> 1 | `CarPartCuller::CullParts`: parts never hidden by view angle (live) |
| 0x620CB9 | imm8 | 4 -> N | car min pixels, Render (live) |
| 0x616343 | imm8 | 4 -> N | car min pixels, RenderFast (live) |
| 0x6156E1 | imm8 | 4 -> N | car min pixels, RenderFlaresOnCar (live) |
| 0x7A66FC | float | 18.0 -> N | car low-detail pixel limit (live) |
| 0x60D544 | 2 B | `74 1E` -> `EB 1E` | `ShouldCarBeRendered`: respawn blinking off (live) |

## fullmap (`src/fullmap.cpp`)

| Address | Kind | Bytes / target | What it does |
|---|---|---|---|
| 0x5E99C0 | JMP, 6 B | `81 EC 00 01 00 00` | `DrawVisibleSections` replaced: vanilla list + extras, capture, guard radius |
| 0x57FA11 | call, 5 B | `E8 1A 06 EA FF` (traffic step 0x420030) | Loader step 7: original step, then `TrackStreamer_BlockUntilLoaded` 0x5E79D0 and the batch build |
| 0x57908F | int | 0x171E8 -> 0x181E8 | Replay snapshot buffer allocation +4 KB |
| 0x5790AB | int | 0x17000 -> 0x18000 | Replay snapshot buffer data size +4 KB |
| 0x440DF0 | hook | `bNewSlotPool` | World slot pools raised to whole-region counts |
| 0x5CC3BD | 2 B | `75 1A` -> `EB 1A` | Mesh loader creates vertex buffers at load, never at the first draw |
| 0x491860 | hook | `RenderWorldFlares` | World flares of every drawn section, not only the camera zone list |
| 0x5D6646 | 3 B | `83 F8 01` -> `83 F8 FF` | `AddInstance`: `px <= 1` cull off (live, only while capture or guard radius is active) |

Not a code patch: 2048 section slots are appended to the free list of TheVisibleSectionManager (sentinel 0x88A284) on
the first track load.

## batch (`src/batch.cpp`)

| Address | Kind | What it does |
|---|---|---|
| 0x494DD0 | hook | Texture pack chunk unloader: batches using a texture of the chunk are dropped before it is freed |

## pools (`src/pools.cpp`)

| Address | Kind | Bytes / value | What it does |
|---|---|---|---|
| 0x44055E | 2 B | `7C 09` -> `72 09` | LAA: `bFree` override range, lower bound |
| 0x440567 | 2 B | `7C 44` -> `72 44` | LAA: `bFree` override range, upper bound |
| 0x5DA4E3 | 2 B | `7C 06` -> `72 06` | LAA: `StreamHeap_Alloc` hint, lower bound |
| 0x5DA4E9 | 2 B | `7C 4E` -> `72 4E` | LAA: `StreamHeap_Alloc` hint, upper bound |
| 0x719C5C | 2 B | `7C B4` -> `72 B4` | LAA: VP6 movie scaler |
| 0x719D30 | 2 B | `7C A8` -> `72 A8` | LAA: VP6 movie scaler (MMX) |
| 0x76EC86 | 2 B | `0F 8D` -> `0F 83` | LAA: D3DX MMX box filter |
| 0x76ECA2 | 2 B | `7D 71` -> `73 71` | LAA: D3DX MMX box filter |
| 0x76ED13 | 2 B | `7C 8F` -> `72 8F` | LAA: D3DX MMX box filter |
| 0x76ED27 | 2 B | `0F 8C` -> `0F 82` | LAA: D3DX MMX box filter |
| 0x79DC54 | 3 x int | 60000 / 43000 / 36000 KB | Streaming pool sizes; city and two-player entries scaled |
| 0x5E4ED7 | call, 5 B | `E8 94 FC FF FF` (`call StreamHeap_ctor` 0x5E4B70) | Constructor, then 1024 extra block nodes on the heap's free list |
| 0x7FA9D4 | 3 x int | 11000 / 9000 / 16000 KB | Car loader pack pool sizes |
| 0x48CD62, 0x48CD6C, 0x48CD91, 0x48CDA2 | int | 0x64000 | ePoly / FrameMalloc buffer size (all four or none) |
| 0x7F917C | int | 400000 | Front-end pool size (bytes) |
| 0x5E9A35 | int | 0x876DE0 -> own buffer | Scenery draw list start |
| 0x5E9A55 | int | 0x8832C0 -> own buffer end | Scenery draw list end |

Wanted list (TrackStreamer+0x29C, `disp32` 0x29C -> 1024-entry array of the plugin), 14 operands:

| Operand | Instruction |
|---|---|
| 0x5D5ED1 | 0x5D5ECF |
| 0x5D60B1 | 0x5D60AF |
| 0x5D61A9 | 0x5D61A7 |
| 0x5DA947 | 0x5DA945 |
| 0x5DAC7B | 0x5DAC78 |
| 0x5DAD40 | 0x5DAD3D |
| 0x5DAEF8 | 0x5DAEF6 |
| 0x5DF16C | 0x5DF16A |
| 0x5DF190 | 0x5DF18D |
| 0x5DF5A3 | 0x5DF5A1 |
| 0x5DF749 | 0x5DF747 |
| 0x5E6278 | 0x5E6276 |
| 0x5E8408 | 0x5E8406 |
| 0x5E84ED | 0x5E84EB |

Deferred list (TrackStreamer+0x188, `disp32` 0x188 -> 1024-entry array of the plugin), 4 operands:

| Operand | Instruction |
|---|---|
| 0x5DA901 | 0x5DA8FF |
| 0x5DAC63 | 0x5DAC61 |
| 0x5DF142 | 0x5DF13F |
| 0x5E7CF4 | 0x5E7CF2 |

The 14 + 4 operands and the constructor call are applied all-or-nothing.

## memfix (`src/memfix.cpp`)

| Address | Kind | Bytes / target | What it does |
|---|---|---|---|
| 0x5BA6C2 | JMP, 11 B | `A1 7C 09 87 00 8B 10 50 FF 52 08` | `eReleaseRenderTargets`: back buffer 0x87097C released only if it exists; resumes at 0x5BA6CD |
| 0x75C6DA | hook | CRT `free` | Low-heap blocks go back to the low heap (LAA only) |
| 0x575621 | rel32 (thunk at 0x575620) | `malloc` 0x75C6C8 -> plugin | `operator new`: callers in 0x63C000 .. 0x6E0000 (D3DX) get memory below 2 GB (LAA only) |
| 0x671D1D | call, 6 B | `FF 15 F4 31 78 00` (`call [VirtualAlloc]`) | D3DX private arena reserved below 2 GB (LAA only) |
| 0x440390 | hook | `bWareMalloc` | Pool lock |
| 0x440540 | hook | `bFree` | Heap guard and pool lock |
| 0x7C2E38 | vtable slot | 0x6F69E6 -> plugin | File system open (slot 3) under a lock, empty free list handled |
| 0x7C2E3C | vtable slot | 0x6F69B2 -> plugin | File system release (slot 4) under the same lock |

## settings (`src/settings.cpp`)

| Address | Kind | Bytes | What it does |
|---|---|---|---|
| 0x5D2850 | JMP, 5 B | `A0 D1 09 87 00` | `RenderFrame` entry: forced hidden engine settings re-applied every frame |

The settings themselves are plain globals (0x870CC0 .. 0x870D10) written directly, not registered sites.

## refl (`src/reflections.cpp`, Vanilla profile, startup)

Registered only when a size is changed, HD Reflections is not installed, and every value is still the original.

| Address | Original | What it does |
|---|---|---|
| 0x5BA08A, 0x5BA08F | 0xF0, 0x140 | Road reflection texture size (x scale) |
| 0x5BA0CC, 0x5BA0D1 | 0xF0, 0x140 | Road reflection depth surface size |
| 0x5C2366, 0x5C236D | 0x140, 0xF0 | Road reflection view size |
| 0x7FEE6C | 128 | Car env-map cube face size |
| 0x7FEE80, 0x7FEE84 | 256 | Rear-view mirror size |

## despawn (`src/despawn.cpp`, live)

| Address | Instruction | Original operand | What it does |
|---|---|---|---|
| 0x40981E | 0x40981C `fcomp` | 0x78429C (250.0) | Traffic teleport distance -> plugin float |
| 0x40BFF5 | 0x40BFF3 `fcomp` | 0x78429C (250.0) | Encounter car deactivation distance -> plugin float |
| 0x490F63 | 0x490F61 `fld` | 0x78D798 (32000) | Light flare fade start (squared distance) |
| 0x490F6C | 0x490F6A `fld` | 0x78D79C (40000) | Light flare fade end (squared distance) |

## perf (`src/perf.cpp`, measurement only)

Entry hooks that take two timestamps each; nothing is changed.

| Address | Function |
|---|---|
| 0x57EAD0 | `FrameLimiter_Wait` |
| 0x609CE0 | `World::DoTimestep` |
| 0x5663B0 | `FEngUpdate` |
| 0x44EFE0 | `UpdateCameraMovers` |
| 0x483B40 | `EAXSound::Update` |
| 0x5E79A0 | `TrackStreamer::ServiceNonGameState` |
| 0x5E6A70 | `TrackStreamer::HandleLoading` |
| 0x57EF60 | `ServiceResourceLoading` |
| 0x5E7AF0 | `SwitchZones` |
| 0x5CAAD0 | `eDisplayFrame` |
| 0x5B9C20 | `BuildCullLists` |
| 0x5814D6 | call site of `RenderFrame` in the main loop (`E8 75 13 05 00`) |

## Hooks outside the register

These are not in SPEED2.EXE code and are not registered:

- The `Direct3DCreate9` and `DirectInput8Create` imports of SPEED2.EXE are redirected (both are `jmp [IAT]` thunks in
  the game, 0x63BD30 and 0x6D7100).
- `IDirect3D9::CreateDevice` (vtable slot 16) on the created object; after it returns, the forced settings are applied
  (after the registry is read, before the render targets are created).
- Device `EndScene` (42) and `Reset` (16) through MinHook for the menu; `Present` (17), `DrawIndexedPrimitive` (82)
  and `DrawPrimitive` (81) for measurement. The menu only draws for the `EndScene` call from `RenderFrame` (return
  address 0x5D2B76).
- DirectInput `GetDeviceState` / `GetDeviceData` inline, and the input guard's import patches in input plugins, to
  block game input while the menu is open.
