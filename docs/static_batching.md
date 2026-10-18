# Static batching (Vanilla profile only)

Target: SPEED2.EXE v1.2 (32-bit, image base 0x400000).

With the full map on ([full_map.md](full_map.md)), the far "extra" sections add tens of thousands of draw calls.
Measured: up to 35 000 draws per frame at about 1.6 us of CPU each. The frame time is the draw submission; the GPU is
idle. The extras are static world geometry, so the plugin merges them once, in the loading screen, into a few
thousand meshes of its own.

Code: `src/batch.cpp`, called from `src/fullmap.cpp`.

## Why never under RTX Remix

Merged meshes are new geometry with new hashes, and Remix replacements and lights are keyed on the original meshes.
Batching is therefore hard-off in the Remix profile (every entry point checks the profile) and on by default only in
the Vanilla profile. It is also off in split screen.

## What is merged

A **group** is the key

- 512 m cell of the section centre (from the section's root box),
- size class of the instance radius `r = SceneryInfo+0x3C + 6`: <= 8, <= 16, <= 32, <= 64 m, larger,
- a "sway" bit (trees, below),
- the mesh effect: WORLD (id 0) or WORLDREFLECT (id 2).

One mesh is built per group, split when it would exceed 65 534 vertices (`FlushRenderLists` 0x5CF655 draws only if
the vertex count is below 0xFFFF) or 256 textures (the part's texture slot is a byte). Items inside a group are
sorted by texture, then section, instance and part.

For each batchable instance the model is the one the LOD hook would draw (LOD A, bound on the spot if needed; see
[lod_a.md](lod_a.md)). Its vertices are copied and transformed to world space:

- Source: the mesh's system-memory vertex data (mesh +0x28), or a read-only lock of its managed vertex buffer.
- Layout: stride 0x18 = float3 position, D3DCOLOR (baked light), float2 UV. This stride is a property of the effect;
  WORLD and WORLDREFLECT both use it.
- Transform: `p' = x*row0 + y*row1 + z*row2 + T`, where the 3x3 rows are the shorts at instance +0x2C divided by 8192
  and T is the float3 at instance +0x20 (as `SceneryInstance_BuildRotation` 0x5D6240 / `SetPosition` 0x5D6210).
  Instances with bit 0 of byte +0x1B (flag 0x100, world pieces) are already in world space and are copied unchanged.
- Only the vertices a part actually uses are copied (remapped); indices are 16-bit triangle lists.

The buffers are created like the game's own: `CreateVertexBuffer(n * 0x18, WRITEONLY, 0, MANAGED)` and
`CreateIndexBuffer(m * 2, WRITEONLY, INDEX16, MANAGED)`. Managed resources survive a device reset.

The plugin's eModel / eSolid / eMesh objects live in its own memory, are self-linked, and are never put into a game
list (unbound models 0x82DBC0, all meshes 0x872ED0, loaded solids 0x835794). The game can neither find, rebind nor
free them.

### Never batched

These stay per instance and are walked through `ScenerySection_AddInstance` 0x5D6570 like any instance:

| Excluded | Why |
|---|---|
| instance flags 0x02 / 0x04 / 0x08 / 0x10 | hidden, never in the player view, or direction-dependent |
| every instance in the scenery override table (0x883DF0, count 0x883DF4) | flag 0x10 is toggled at run time (groups, smokeables, respawn) |
| solid flag 0x40 without instance flag 0x100 | uses its own light context |
| solids with marker flares (+0x1B > 0) without flag 0x100 | the culler emits marker flares per instance |
| CAR / WORLDBONE meshes (mesh flags & 0x81), effects other than 0 / 2 | other stride, light context and material |
| part flag bit 0 | second matrix / bones |
| SKYDOME hash 0xBE43EDBB, 0x72923C81, models with a texture replacement table (+0x10) | special render paths |
| textures still pointing at the default TextureInfo 0x82DB08 | patched later by the game |
| panoramas, `never` sections, 26xx | not extras |

## How the batches are drawn

The batches do not bypass the renderer. Each one is written as a normal 12-byte entry into the player view's
visibility list, exactly where the extra sections write theirs:

```
{ eModel* | visible state (1 partial, 2 fully inside), matrix = 0, &fakeInstance }
```

The game then draws it itself: `SceneryRender` 0x5D6AD0 -> `Render` 0x5C5930 (matrix 0 = identity 0x872E90) ->
`eAddRenderEntry` 0x5CEB30 -> `FlushRenderLists` 0x5CF420. Effect, texture state, blend / alpha / Z-write, fog and
render-list sorting work unchanged. `SceneryRender` reads only flags +0x1A of the instance, so one static 0x40-byte
fake instance with flags 0x0100 (world piece) serves all batches. Each part becomes one render entry and one draw
call, and `Render` tests each part's world AABB against the frustum when the state is 1.

Per frame, only in the main player view (view 1):

1. For each extra section that has batch contributions, only its leftover (non-batched) instances are walked through
   `AddInstance`. Sections without contributions use the normal walk.
2. A part is one run of consecutive contributions of the same texture whose sections are extras in the current zone.
   Parts are rebuilt only when the set of active sections changes (a hash over the section numbers; in practice on a
   zone change).
3. Each mesh is culled by
   - class distance: nearest point of the mesh box farther than `rHi * (1 + scale / limit)` is skipped, with `scale`
     = cull info +0x80 and `limit` = max(world limit, extra limit, 1). The top class has no distance limit. This
     replaces the per-instance pixel test with a conservative bound per class.
   - frustum: `GetVisibleState` 0x48AC60 on the mesh box -> 0 skip, 1 partial, 2 inside.
4. The mesh effect is refreshed from the effect table 0x86EB28 every frame (effect objects can be recreated).

Extras are drawn without precull by default, and so are the batches.

## When it is built

In the loading screen, right after `BlockUntilLoaded` (the redirected call at 0x57FA11): everything is loaded and
activated and no frame is running. It is rebuilt at every load; the previous batches are freed first. Switching
`StaticBatching` on therefore takes effect at the next load; switching it off is live (the normal per-instance path is
used).

## Budget

`BatchBudgetMB` (default 128, range 16-512) limits the merged buffers. Managed resources keep a copy in system memory
as well, so the same amount is used again in RAM. Groups are ranked by "draws saved per byte"
(`(items - distinct textures) / bytes`) and accepted in that order until the budget is full. Instances of rejected
groups stay per instance. Groups that save nothing are never built.

Offline estimate for all batchable instances of all real sections (trees not included): about 96 MB for L4RA and for
L4RD (82 MB vertices, 14 MB indices). Smaller regions are far below.

## Trees within 200 m

Swaying trees (solid flag 0x80, instance not a world piece) sway through a per-instance matrix. With `BatchTrees` on
(default) they go into their own groups:

- If the group box is nearer than 200 m to the camera, its trees are drawn per instance (swaying) and the merged mesh
  is skipped for that frame.
- Farther away the merged mesh is drawn and the trees stand still. Vanilla sways about +-1.5 degrees at the tip, which
  is not visible at that distance.

With `BatchTrees` off, swaying trees are never batched.

## Safety

- **Texture packs.** The only game pointers a batch touches while drawing are its TextureInfos. The texture pack chunk
  unloader 0x494DD0 is hooked: before a pack goes, every batch with a TextureInfo inside that chunk is killed (buffers
  released, no parts), and its sections fall back to the per-instance walk.
- **Reloaded sections.** If a section's header pointer differs from the one recorded at build time, that section falls
  back to the per-instance walk for the rest of the session.
- The game's own solids and meshes are not referenced at draw time (the geometry was copied), so their unloading is
  harmless.
- A build failure (exception) leaves everything per instance.

## Expected effect (offline estimate)

Per camera zone, extras only, no frustum, pixel limit 4 (plugin default) / 17 (vanilla), mean draw calls:

| Variant | L4RA, limit 4 | L4RA, limit 17 | L4RD, limit 4 | L4RD, limit 17 |
|---|---|---|---|---|
| per instance | 42 199 | 10 993 | 44 095 | 11 885 |
| 512 m cells + size classes | 11 173 | 3 734 | 11 742 | 3 846 |
| + trees beyond 200 m batched | 6 813 | 3 340 | 6 999 | 3 433 |

The floor is the number of distinct textures per cell. The menu and the log show the real numbers in game: meshes
drawn, culled by class distance, culled by frustum, leftover instances, near tree groups, dead meshes and fallback
sections.
