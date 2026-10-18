# Full map

Target: SPEED2.EXE v1.2 (32-bit, image base 0x400000, no relocations). All addresses are virtual addresses in that image.

Vanilla NFSU2 streams the city in zones. Around the camera it draws the real sections of the camera zone's list;
everything else is either not loaded or replaced by coarse stand-ins ("panoramas"). "Full map" loads the whole region,
activates it in the loading screen, and draws the real geometry (in LOD A, see [lod_a.md](lod_a.md)) instead of the
stand-ins wherever that is safe.

Code: `src/fullmap.cpp`, `src/fullmap_tables.h`, `src/fullmap_replace.h`, `src/pools.cpp`, the wanted-list hook in
`src/lod.cpp`, `src/despawn.cpp`.

## Profiles

| Profile | Streaming | Drawing |
|---|---|---|
| Vanilla | whole region | vanilla list + every other loaded real section ("extras"), optional static batching |
| RTX Remix, capture off | exactly vanilla | exactly vanilla (only the LOD choice changes), plus the guard radius |
| RTX Remix, capture on | whole region | every object of every loaded section except panoramas |

Full map needs the streamer limits (below). If they could not be applied, or if any of the four core sites
(0x5E99C0, 0x57FA11, 0x57908F, 0x5790AB) does not hold the original bytes, full map stays off and the game behaves
as vanilla. The slot-pool hook (0x440DF0) is required as well; without it the world pools would overflow.

## 1. Stream in everything

The game builds its wanted list in `BuildWantedList` 0x5E5110. Its only caller is `SwitchZones`
(call at 0x5E7BEC, `ecx` = TheTrackStreamer 0x88EB00). The call is redirected to a wrapper:

1. The original `BuildWantedList` runs.
2. The unit table is read at run time: TrackStreamer `[0]` is an array of 0x50-byte units, `[1]` the count, the unit
   number is a `short` at +0x08 (number / 100 = letter, 1..26 = A..Z).
3. Every unit of the region is added through `AddSectionsToLoad` 0x5DACB0
   (`thiscall(ts, int16* list, int count, int slot)`, `ret 0Ch`), once for slot 0 and once for slot 1, like the
   game's own fixed list. Excluded: X/Y units 50 and above (the split-screen set).
4. Nothing is added in split screen (`[0x89E7C0] == 2`).

`AddSectionsToLoad` writes back into the list it is given (in two-player mode it remaps X/Y numbers by +50), so it
always gets a writable copy.

The same hook also implements "LOD A units always wanted" when full map is off (only X/Y < 50 are added then).

## 2. Per-region rule tables

Drawing every loaded section blindly gives overlaps: panoramas and real sections occupy the same space, some
instances exist twice. The rules that avoid this were generated offline from the game's track data and are compiled
in (`fullmap_tables.h`). Per region:

| Array | Meaning |
|---|---|
| `pan` | panorama sections (stand-ins); never drawn as extras |
| `never` | sections that are never drawn as extras |
| `blocked` + `zone` | per camera zone, the real sections that must not be drawn as extras. `zone` rows are `{zone, start, count}` into `blocked` |
| `crc` | CRC32 of the region's zone-list chunk 0x34153, recorded by the generator |

The blocked list of a zone holds three kinds of section: real sections within 8 m of a panorama that is shown in that
zone, sections whose instances duplicate others, and sections that clearly overlap shown geometry. Nothing vanilla
draws is ever dropped; the rules only limit what is added.

| Region | Zones | Panoramas |
|---|---|---|
| L4RA | 255 | 23 |
| L4RB | 15 | 3 |
| L4RC | 24 | 0 |
| L4RD | 255 | 23 |
| L4RF | 16 | 4 |
| L4RG | 26 | 4 |
| L4RH | 4 | 0 |
| L4RR | 4 | 0 |

**Region selection.** The region name is read from `TrackInfo::LoadedTrackInfo` (pointer at 0x883DAC, name at +0x40)
and matched case-insensitively against the table names. A region without a table (a mod map with its own name) gets
no rules: nothing extra is wanted, nothing is activated in the loading screen, and drawing is exactly vanilla. The
current code matches by name only; the recorded CRC values are not compared at run time.

## 3. Panorama replacement

`fullmap_replace.h` is a second rule set for "replace mode" (`ReplacePanoramas`, on by default, switchable live):

- `rdrop`: city panoramas that are fully covered by real geometry. In replace mode they are skipped even when they
  are on the vanilla list, and their real sections are drawn instead.
- `rblocked` + `rzone`: the reduced per-zone blocked lists for this mode. Only the blocks belonging to panoramas that
  are kept (terrain, ocean, landmarks without a real counterpart) and the duplicate/overlap blocks remain.

| Region | City panoramas replaced | Zone blocks lifted | Blocks kept |
|---|---|---|---|
| L4RA | 14 | 52 605 | 35 |
| L4RB | 0 | 138 | 0 |
| L4RD | 15 | 43 065 | 53 |
| L4RG | 2 | 68 | 0 |
| others | 0 | 0 | 0 |

Replace mode is applied only in player views and only while extras are drawn: a panorama is never hidden without its
real sections being drawn in its place.

## 4. Streamer limits

Loading a whole region breaks several fixed-size structures of the streamer. All of them are fixed in `pools.cpp`
and applied all-or-nothing (a half-moved list is worse than none).

**Stream heap block nodes.** The stream heap has 192 block nodes inline (heap +0x1C .. +0xF1C), taken without an empty
check (0x5DA5C1). Enlarging the inline array would overlap the pool override data at +0xF1C. Instead, the call to
`StreamHeap_ctor` 0x5E4B70 at 0x5E4ED7 (inside `InitMemoryPool` 0x5E4E70) is redirected: after the constructor, 1024
nodes of the plugin (`{next, prev, addr, size, used}`, 0x14 bytes, the constructor's own layout) are appended to the
free list at heap +0x0C. The destructor 0x5E4D70 only empties the sentinels and never frees a node, so the same
array is reused at the next `InitMemoryPool`.

**Wanted and deferred lists.** The wanted list (256 x 4 bytes at TrackStreamer+0x29C) and the deferred list
(64 x 4 bytes at +0x188) have no bound check (`AddSectionsToLoad` 0x5DACB0, `DeferUnit` 0x5DF120). Behind them lie the
heap pointer (+0x69C) and the lock bytes (+0x288). Both lists are moved to 1024-entry arrays of the plugin by
rewriting their 14 + 4 `disp32` operands (all in TrackStreamer methods, base register always TheTrackStreamer
0x88EB00). The counters stay where they are. A sweep of every memory operand with a displacement in the affected
range found no other access; the only `memset` (0x5E7CED, 64 entries at +0x188) is harmless.

**Streaming pool.** Size table 0x79DC54 = 60000 / 43000 / 36000 KB (city / two players / L4RG). Entries 0 and 1 are
scaled by `StreamPoolPercent` (default 200), entry 2 stays. L4RA needs about 101 MB; 200 % gives 117 MB. Above 200 %
the city lights vanish after a shop visit, so 200 is the practical maximum.

**World slot pools.** The world pools have fixed counts (`bMalloc` 0x43F8F0 never grows them) and the game does not
check for null everywhere. With the whole region loaded `TexturePackSlotPool` ran out and the texture pack header
loader 0x494380 crashed at 0x491A00. `bNewSlotPool` 0x440DF0 is hooked and raises these pools to a minimum count:

| Pool | Vanilla | Full map |
|---|---|---|
| TexturePackSlotPool | 128 | 2048 |
| eStreamingPackSlotPool | 64 | 1024 |
| eModelSlotPool | 5000 | 65536 |
| eTextureBucketSlotPool | 1024 | 32768 |
| eMeshRender | 4096 | 262144 |
| eLightMaterialPlatInfoSlotPool | 220 | 2048 |
| eAnimTextureSlotPool | 256 | 2048 |
| TopologyTreeSlotPool | per region | 1024 |
| WorldModelSlotPool | 160 | 1024 |
| SpaceNodeSlotPool | 224 | 1024 |

Running out of eModel slots would silently drop LOD models, so that pool gets the most room. eMeshRender has to hold
every part of every instance of one frame in capture mode.

**Section slots.** TheVisibleSectionManager 0x883E70 has 512 inline slots (0x1C bytes each) on a free list whose
sentinel is at 0x88A284. The function 0x5DB680 takes one slot per activated section without an empty check. L4RA has
606 sections; the 513th "slot" was the sentinel itself (crash at 0x5DB6A3). Slots are only reached through that list
and the slot table 0x883EC0, never by index, so 2048 slots of the plugin are appended to the tail of the free list
once, on the first track load. This is a data-list change, not a code patch.

## 5. BlockUntilLoaded in the loading screen

Vanilla activates one drivable unit per frame after the race has started. With the whole region wanted, that would
take seconds of visible pop-in. The call at 0x57FA11 (loader step 7, originally the streaming traffic car manager
step 0x420030) is redirected: the original step runs, then `TrackStreamer_BlockUntilLoaded` 0x5E79D0 is called on
0x88EB00. Everything wanted is read and activated before the loading screen ends. The log line reports wanted,
activated and skipped units (TrackStreamer +0x298, +0x24, +0x28). Static batching is built right after this
([static_batching.md](static_batching.md)).

**Vertex buffers at load.** The mesh loader 0x5CC280 defers `CreateVertexBuffer` to the first draw
(`FlushRenderLists` 0x5CF5AA: create, lock, copy) while the race coordinator is in state 4. With the whole map loaded,
hundreds of those creations landed mid-drive at once (measured: 250-336 ms frames). The `jnz` at 0x5CC3BD becomes a
`jmp`, so the buffers are always created while loading.

**Replay buffer.** A replay snapshot stores one byte per wanted drivable unit. The snapshot buffer is enlarged by
4 KB: allocation size at 0x57908F (0x171E8 -> 0x181E8) and data size at 0x5790AB (0x17000 -> 0x18000).

## 6. Draw list size

Vanilla has one scenery draw list of 4200 x 12-byte entries for all views together (0x876DE0 .. 0x8832C0). When it is
full, further instances are dropped silently. It is moved to a `VirtualAlloc` buffer of the plugin by rewriting the
two immediates at 0x5E9A35 (start) and 0x5E9A55 (end). It is never extended in place, because 0x8832C0 is the precull
"BooBoo" map. Size = 4200 x `DrawListPercent` / 100: 400 % by default, 3000 % in the Remix profile (capture mode puts
every instance of the region into one frame, about 48 000 in L4RA). The consumers compare the cursor unsigned
(`jae` at 0x5D6751, 0x5D67BB, 0x5D6D4C), so a buffer above 2 GB is fine.

Related: the ePoly / FrameMalloc buffers (2 x 0x64000 bytes; immediates at 0x48CD62, 0x48CD6C, 0x48CD91, 0x48CDA2) hold
a matrix for every drawn instance and car. When they run dry, instances and whole cars vanish silently. Default
400 %, Remix profile 2000 %.

## 7. Drawing: the DrawVisibleSections replacement

`DrawVisibleSections` 0x5E99C0 is replaced (6-byte JMP) and runs once per view (cull info). Cull info fields used:
+0x40 eView*, +0x44 pass flags (0x6000 = player view, 0x1800 = reflection / env), +0x48 / +0x4C / +0x50 list start,
cursor, end, +0x60 camera position, +0x80 LOD scale, +0x84 precull cell (-1 = off).

1. **Vanilla list.** `GetVisibleSectionList` 0x5E96B0 (the 26xx sections + the camera zone's list), with room for 1024
   entries instead of 128. Every listed section is walked through the game's cull tree walk 0x5D69B0, precull as in
   vanilla. In replace mode, `rdrop` panoramas are skipped here.
2. **Extras** (player views only, Vanilla profile, `DrawExtraSections` on): every section on the loaded list
   0x88EAD4 except 26xx, the listed ones, panoramas, `never`, and the zone's blocked list. Each section is walked at
   most once per view (the loaded list can link a re-activated section twice).
   - Precull is off for extras by default: the 32 m precull cells wrap every 1024 m, so their bits carry no meaning
     for sections of other zones. `ExtraPrecull` turns it on (50-90 % fewer objects, with some wrap noise).
   - `ExtraMinPixelSize` gives extras their own pixel limit (0 = same as the world limit).
3. **Camera outside every zone.** If `FindDrivableSection` 0x5E00D0 finds no zone for the camera (free camera tools,
   high replay cameras), vanilla lists every loaded xx00..xx09 section and the rules have no zone, which showed black
   ground and sea. The last zone the camera was in is used instead, as if the camera were still there, with precull
   off for that view.
4. **BooBoo map.** `TRACKS\PrecullerBooBooScript.hoo` is loaded to 0x8832C0 (128 x 128 bits, 32 m cells). In a listed
   cell `BuildCullLists` switches precull off for every view (test at 0x5E9B54). The `jz` at 0x5E9B5A becomes a `jmp`,
   so precull stays on (`PrecullBooBooOff`). EA's precull data of those cells hides the panoramas, so inside a BooBoo
   cell the panorama sections alone are still walked without precull.

## 8. Capture mode (RTX Remix)

For an RTX Remix capture (`RemixCaptureAll`, Remix profile only, live): the whole region is streamed in and every
instance of every loaded section except panoramas and `never` is offered to the culler through
`ScenerySection_AddInstance` 0x5D6570 with visible state 2 (no frustum test), precull off, and no pixel limit. The
`px <= 1` cull at 0x5D6646 (`cmp eax,1` -> `cmp eax,-1`) is switched off while capture or the guard radius is active;
behind the camera the pixel size is 0. Switching capture on or off sets TrackStreamer+0x288 (0x88ED88) to 1, so
`DetermineZones` reports a change and `SwitchZones` -> `BuildWantedList` runs with the new rule. The log reports the
draw-list fill and warns when it is full.

## 9. Guard radius and precull-off radius (RTX Remix)

Remix lights and shadows need geometry beside and behind the camera. In the Remix profile without capture
(`GuardRadius`, default 100 m; `PrecullOffRadius`, default 200 m; 0 = vanilla), a listed section whose root box lies
within the larger of the two radii is walked instance by instance instead of through the cull tree:

| Instance box distance to camera | Frustum | Precull | Pixel limit |
|---|---|---|---|
| <= GuardRadius | none (state 2) | off | none (the `px <= 1` cull is off too) |
| <= PrecullOffRadius | normal | off | normal |
| farther | normal | as vanilla | normal |

Farther sections use the game's own walk. The guard radius never adds sections; it only keeps instances of sections
vanilla already draws.

## 10. World flares over the drawn sections

`RenderWorldFlares` 0x491860 draws lamp and neon flares, but walks only the camera zone's list, so far sections
drawn as extras had no lights. The function is hooked: while extras (Vanilla) or capture (Remix) are drawn, it walks
exactly the sections drawn in the last main player view (view 1). Per section the flare pack hangs at slot+8; the
pack box (+48 / +64) is tested with `GetVisibleState` 0x48AC60, then every flare on the list at pack+88 goes through
`eRenderLightFlare` 0x490920 with the identity matrix 0x872E90. Outside free driving / racing (game state 6), with
flares off (`[0x7F3D40] == 0`), or in the Remix profile without capture, the game's own function runs.

## 11. Despawn and flare distances

Both are live and apply to both profiles (default 400 m each).

- **Traffic.** `TrafficTeleporter_IsWithin250mOfPlayer` 0x409800 (`fcomp [0x78429C]` at 0x40981C): a traffic car
  farther than this path distance from the nearest player may be teleported ahead, i.e. it vanishes.
- **Encounter cars.** `RandomEncounterManager_DeactivateFarCars` 0x40BFB0 (`fcomp [0x78429C]` at 0x40BFF3) deactivates
  them beyond the same distance.
- The 250.0 at 0x78429C has four more readers, so it is never written. Only these two operands are pointed at a float
  of the plugin.
- **Light flares.** `eRenderLightFlare` 0x490920 fades main-view and world flares between squared distances 32000 and
  40000 (about 179 to 200 m; `fld [0x78D798]` at 0x490F61 and `fld [0x78D79C]` at 0x490F6A, each the only reader).
  Both operands are pointed at the plugin's own pair. The fade start keeps the vanilla ratio (0.8944 x end).
