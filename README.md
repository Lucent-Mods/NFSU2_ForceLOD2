# NFSU2 ForceLOD 2

Full detail, the whole city and RTX Remix support for **Need for Speed Underground 2**.

Underground 2 swaps every object to a cruder model a few metres away, streams only the blocks around
your car, paints the rest of the skyline with flat panorama stand-ins, culls whatever it thinks you
cannot see, and removes traffic and street-lamp flares early. That was the right call on a 2004 PC.
ForceLOD 2 turns all of it off:

- every object and every car is drawn with its best model (**LOD A**) at any distance,
- the **whole region** is loaded, and the real far geometry is drawn instead of the panoramas,
- far sections are **merged into a few big meshes** so a modern GPU draws all of it at high frame rates,
- traffic, encounter cars and light flares stay visible twice as far,
- several crashes of the game itself are fixed.

The same file is also a compatibility layer for **[RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix)**:
one mesh per object (no LOD switches, so geometry hashes stay stable), nothing culled near the camera
(Remix keeps the lights and shadows of what is beside and behind the car), no extra views that render
the scene a second time, and a capture mode that draws an entire region for a Remix capture.

> The plugin file is called `NFSU2_ForceLOD2.asi`; that is the name you will see in the game folder,
> the INI and the log. It replaces the original ForceLOD – do not run both.

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Quick start](#quick-start)
- [Profiles](#profiles)
- [The menu](#the-menu)
- [Configuration](#configuration)
- [RTX Remix setup](#rtx-remix-setup)
- [Compatibility](#compatibility)
- [How it works](#how-it-works)
  - [Source layout](#source-layout)
- [Building](#building)
- [Antivirus false positives](#antivirus-false-positives)
  - ["Keylogger" / keyboard capture](#keylogger--keyboard-capture)
- [Troubleshooting](#troubleshooting)
- [Known limits](#known-limits)
- [Credits](#credits)

## Features

- **LOD A everywhere** – scenery, cars and tyres always use their most detailed model. If an object's
  LOD A is not in memory yet it is skipped for that frame instead of being drawn as LOD B, so an object
  never changes its mesh.
- **Full map** – the whole region is streamed in before the race or free roam starts. Real sections are
  drawn everywhere, city panoramas that are covered by real geometry are replaced by it. Terrain and
  ocean panoramas without a real counterpart stay. Regions the plugin has no rule table for (new mod
  maps) are drawn exactly like vanilla.
- **Static batching** (Vanilla profile) – in the loading screen the far sections are merged into static
  meshes: thousands of draw calls fewer. Trees within 200 m are still drawn one by one and sway.
- **Guard radius** (Remix profile) – within 100 m of the camera nothing is culled: not beside, not behind
  the camera, not by the game's occlusion data, not by size. The game's occlusion data is ignored within
  200 m.
- **Capture mode** (Remix profile) – the whole region, every object, no occlusion, no view frustum, no
  pixel limit, panoramas left out. For RTX Remix captures.
- **Later despawn** – traffic and encounter cars are removed beyond 400 m (vanilla 250 m), light flares
  fade out up to 400 m (vanilla 200 m), including the flares of the far sections.
- **Larger reflections** (Vanilla profile) – road reflection up to 1280×960, car reflection cube up to
  512, rear-view mirror up to 1024.
- **Forced engine settings** – motion blur, light streaks, depth of field, fog, rain and more, per profile.
- **Bigger memory pools** – streaming, car loader, per-frame matrices, draw list and the streamer's own
  fixed-size lists, so the full map fits.
- **Crash fixes of the game** – track unload, multi-core pool races, the shop crash at `0x6F6AF9`, and
  D3DX on a large-address-aware executable.
- **In-game menu** with live counters, frame time per engine stage and a list of every patched address.

## Requirements

- Need for Speed Underground 2, **`SPEED2.EXE` v1.2** with a size of 4,800,512 bytes – the executable most
  mods target. The plugin checks the code it patches at startup; on a different executable it patches
  nothing and says so in the log.
- An **ASI loader**, e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as
  `dinput8.dll` in the game folder. If you already run the widescreen fix or Extra Options, you have one.
  Under RTX Remix the loader must not be `d3d9.dll` – that file is the Remix bridge.
- Recommended for the full map, required for RTX Remix: a **large-address-aware `SPEED2.EXE`**
  ("4GB patch"). The whole map and the Remix bridge need more than 2 GB of address space.

## Installation

1. Download `NFSU2_ForceLOD2_1.0.0.zip` from the [releases](https://github.com/HellRaven-Mods/NFSU2_ForceLOD2/releases)
   or [build it yourself](#building).
2. Copy `NFSU2_ForceLOD2.asi` and `NFSU2_ForceLOD2.ini` into the game's `scripts\` folder.
3. Open `NFSU2_ForceLOD2.ini` and pick the profile: `Profile=Vanilla` for the normal game,
   `Profile=Remix` (the default) for RTX Remix.
4. Start the game and press **Insert**.

To uninstall, delete the `.asi`, the `.ini` and `NFSU2_ForceLOD2.log`. Nothing in the game's own files
is ever modified – every change lives in the memory of the running game.

## Quick start

**Vanilla.** Set `Profile=Vanilla`, start a free roam. The first load takes longer: the whole region is
streamed in and the far sections are merged. Drive somewhere high and look across the city – real
buildings instead of panoramas. *Diagnostics* shows the frame time and draw calls; if your CPU struggles,
raise *LOD > Min pixel size* or switch on *Map > Far sections with precull*.

**RTX Remix.** Set `Profile=Remix`, set up Remix as described [below](#rtx-remix-setup), start the game.
The *Status* tab says "RTX Remix detected" and lists anything in your setup that will break it. Open the
Remix menu (Alt+X) → Debug View "Geometry Hash": world and cars should keep their colours while you
drive. For a capture, switch on *Map > Capture mode*, capture, switch it off again.

## Profiles

`Profile=` picks the start value of every setting at once. Every value can still be changed on its own;
the INI stores only what differs from the profile, so switching `Profile=` in the file switches
everything you never touched.

| | Remix | Vanilla |
|---|---|---|
| LOD A for every object and car | yes | yes |
| Full map | yes | yes |
| Static batching of far sections | **never** – merged meshes would get new hashes | yes |
| Guard radius / no occlusion near the camera | 100 m / 200 m | off (game culling) |
| Capture mode | optional | – |
| Car shadow, headlight projector, motion blur, light streaks, depth of field, overbright, bleach bypass, tinting, horizon fog | off | as in the options menu |
| Reflection sizes | – | selectable |
| Draw list / per-frame matrix pool | 3000 % / 2000 % (capture draws every instance of a region in one frame) | 400 % / 400 % |
| Traffic despawn / light flares | 400 m / 400 m | 400 m / 400 m |

Engine settings are kept per profile (`[Settings.Remix]` / `[Settings.Vanilla]`): a value forced in one
profile never carries over to the other.

## The menu

**Insert** opens and closes it; another key can be picked on the *Status* tab (Insert, Home, End,
Delete, Page Up/Down, Pause, Scroll Lock, `` ` ``, F1–F12). While the menu is open the game receives no
keyboard or mouse input. Changes take effect immediately unless marked *(restart)*; **Save to INI** keeps
them.

| Tab | What is there |
|---|---|
| **Status** | Profile, *Reset to profile values*, RTX Remix detection and setup warnings, back buffer size and fps, and the per-frame LOD counters. Red counters break the rule "one mesh per object" and should be 0. Menu key. |
| **LOD** | World pixel limit and hysteresis, LOD A in reflection views, car and tyre LOD override, cullable car parts, car pixel limits, respawn blinking, the game's "occlusion off" map. |
| **Map** | Full map, drawing the far sections, precull for far sections, capture mode (Remix), panorama replacement, pixel limit of the far sections, static batching with its memory limit and statistics (Vanilla), keeping LOD A units loaded. |
| **Culling** | Guard radius and occlusion radius (Remix), traffic / encounter despawn distance, light flare distance. |
| **Settings** | Forced engine settings ("Game" = the options menu decides), reflection sizes (Vanilla). |
| **Pools** | Memory pool sizes, LAA pointer fix, streamer limits, the crash fixes and their hit counters. |
| **Diagnostics** | Frame time of the last 10 s split by engine stage (simulation, render, cull, present …), draw calls and primitives, and every address the plugin patched – green ours, blue the same patch by another plugin, red owned by another plugin and left alone. |

## Configuration

`NFSU2_ForceLOD2.ini` next to the `.asi`. A commented-out or missing key uses the profile value.
*Live* = changeable in the menu while driving; *startup* / *next load* = takes effect after a restart or
the next loading screen.

| Section | Key | Default | |
|---|---|---|---|
| General | `Profile` | `Remix` | `Remix` or `Vanilla` |
| | `MenuKey` | `45` | virtual key code (45 = Insert) |
| | `Log`, `CrashLog` | `1` | log file, crash report + minidump |
| | `HangWatchdog` | `30` | seconds the streamer may wait in a loading screen before it is logged, 0 = off |
| | `InputBlock` | `1` | the game gets no input while the menu is open |
| | `InputGuardModules` | `NFSU_XtendedInput` | other input plugins that are held back while the menu is open |
| LOD | `WorldMinPixelSize` | `4` | scenery smaller than this on screen is not drawn (game pixel scale, vanilla 17) – live |
| | `WorldPixelHysteresis` | `2` | band above the limit that keeps the previous decision (no flicker) – live |
| | `ReflectionLodA` | `1` | reflection views draw LOD A too – live |
| | `CarLodOverride`, `CarTyreLodOverride` | `0` | 0 = LOD A, -1 = the game decides – live |
| | `CullableCarParts` | `1` | underbody, brakes and side parts never hidden by camera angle – live |
| | `CarMinPixelSize` | `1` | cars smaller than this are not drawn (vanilla 4) – live |
| | `CarLowDetailPixels` | `0` | below this cars lose wheels and brakes (vanilla 18, 0 = never) – live |
| | `CarBlinkOff` | `1` | no blinking of respawning cars – live |
| | `PrecullBooBooOff` | `1` | ignore the game's map of cells where occlusion culling is switched off – live |
| | `KeepLodAUnits` | `1` | keep every shared LOD unit loaded so LOD A always exists – startup |
| Culling | `GuardRadius` | Remix `100`, Vanilla `0` | m around the camera where nothing is culled – live |
| | `PrecullOffRadius` | Remix `200`, Vanilla `0` | m around the camera where occlusion data is ignored – live |
| FullMap | `Enable` | `1` | whole region loaded – startup |
| | `DrawExtraSections` | `1` | draw the far real sections – live |
| | `ExtraPrecull` | `0` | far sections use the game's occlusion data too (50–90 % fewer objects) – live |
| | `ReplacePanoramas` | `1` | city panoramas covered by real geometry are replaced – live |
| | `RemixCaptureAll` | `0` | Remix only: capture mode – live |
| | `ExtraMinPixelSize` | `0` | own pixel limit for far sections, 0 = same as the world limit – live |
| | `StaticBatching` | Vanilla `1` | Vanilla only: merge far sections – next load (switching off is live) |
| | `BatchBudgetMB` | `128` | memory limit of the merged buffers (VRAM, plus a managed copy in RAM) – next load |
| | `BatchTrees` | `1` | batch trees too (beyond 200 m without sway) – next load |
| Despawn | `TrafficDistance` | `400` | m, vanilla 250 – live |
| | `FlareDistance` | `400` | m, vanilla 200 – live |
| Reflections | `RoadReflectionScale` | `1` | Vanilla only: 1 = 320×240, 2 = 640×480, 4 = 1280×960 – startup |
| | `CarEnvMapSize` | `0` | Vanilla only: 0 = 128, or 256 / 512 – startup |
| | `MirrorSize` | `0` | Vanilla only: 0 = 256, or 512 / 1024 – startup |
| Pools | `StreamingPoolPercent` | `200` | world streaming memory, max 200 (above that the city lights vanish after a shop visit) |
| | `PackPoolPercent` | `200` | car loader |
| | `PolyBufferPercent` | `400` | per-frame matrices of drawn objects and cars |
| | `FrontEndPoolPercent` | `100` | menus, for front-end heavy mods |
| | `DrawListPercent` | `400` | scenery draw list (vanilla 4200 entries) |
| | `LaaPointerFix` | `1` | unsigned pointer compares, needed with a large-address-aware executable |
| | `StreamerLimits` | `1` | more stream heap nodes, bigger wanted / deferred lists – needed for the full map |
| Memory | `HeapGuard` | `1` | track unload crash |
| | `PoolLock` | `1` | multi-core crashes in the game's memory pools |
| | `FileGuard` | `1` | shop crash `0x6F6AF9` |
| | `LowMemD3DX` | `1` | D3DX allocations below 2 GB on a large-address-aware executable |
| Settings.Remix / Settings.Vanilla | `CarShadow` `CarHeadlight` `Crowd` `Fog` `MotionBlur` `LightStreaks` `LightGlow` `AnimatedTexture` `ParticleSystem` `DepthOfField` `OverBright` `BleachByPass` `Tinting` `HorizonFog` `Rain` | see [Profiles](#profiles) | -1 = options menu, 0 = off, 1 = on (`CarShadow` 2 = high) – live |

All Pools and Memory values take effect at startup.

## RTX Remix setup

In the game's own display options switch **Car Reflection** and **Road Reflection** off – the game then
never creates those views – and set world, car and texture detail to maximum (texture hashes depend on
the top mip level).

Every known NFSU2 Remix project runs the game in fixed function. The values below are the community's
(from [Ekozmaster/NFSU2-RTX-Remix](https://github.com/Ekozmaster/NFSU2-RTX-Remix) and
[adamplayer/Underground2RTX](https://github.com/adamplayer/Underground2RTX)), not tuned by this plugin.
Both files are in `remix_examples\` in the release.

`dxvk.conf` in the game folder – without `shaderModel = 0` Remix drops the game's draws:

```ini
d3d9.shaderModel = 0
d3d9.customVendorId = 10de
d3d9.customDeviceId = 0110
```

`rtx.conf` in the game folder:

```ini
rtx.useVertexCapture = False
rtx.useWorldMatricesForShaders = False
rtx.fusedWorldViewMode = 2
rtx.zUp = True
rtx.sceneScale = 0.01
rtx.skyDrawcallIdThreshold = 8
rtx.uniqueObjectDistance = 4
rtx.antiCulling.light.enable = True
rtx.antiCulling.light.fovScale = 2
rtx.antiCulling.light.numFramesToExtendLightLifetime = 600
```

At startup the plugin checks `dxvk.conf`, the large-address-aware flag and the profile, and lists any
problem on the *Status* tab and in the log. The menu is drawn with pre-transformed vertices at the end
of the frame, so Remix treats it as UI and never as scene geometry.

## Compatibility

- **Every patch checks the original bytes first.** If another plugin (Unlimiter, Extra Options,
  HD Reflections, …) has already changed an address, ForceLOD 2 leaves it alone and marks it
  *FOREIGN* on the *Diagnostics* tab. If the other plugin wrote exactly the same bytes, it shows as
  *shared* and is never reverted.
- **HD Reflections** installed: its reflection sizes win, ForceLOD 2's own reflection settings are off.
- **Widescreen fix, XtendedInput, LiveTexture Editor**: work alongside. The device is caught
  through the game's own import of `Direct3DCreate9`, not by patching system DLLs, and the keyboard hooks
  sit on the real DirectInput functions, so chains of plugins hooking the same device do not loop.
- **Original ForceLOD**: do not run both – they patch the same places.
- **Mod maps**: the rule tables are picked by region name (`L4RA` …). A region without a table is drawn
  exactly like vanilla, and the log says so (`no rule table: vanilla drawing`).

## How it works

The short version: the plugin never touches vertex data, textures or files. It decides *whether* and
*which* mesh the game draws, *what* the streamer loads, and *how big* the game's fixed-size memory is.

**LOD A.** The world's LOD choice (`0x5D6653`) is replaced: LOD A always, and if LOD A is not bound yet
its model is bound on the spot when its solid is in memory; if not, the instance is culled for that frame – it is never drawn as LOD B. Cars
get the engine's own LOD override (`0x802728` / `0x80272C` = 0). The shared LOD units that hold most
LOD A meshes stay loaded. → [docs/lod_a.md](docs/lod_a.md)

**Full map.** Every unit of the region is put on the streamer's wanted list, the loading screen waits
until all of it is in, and the draw-visible-sections function is replaced: the game's list for the
camera zone plus every other loaded section, by per-region rule tables generated offline from the
game's track data. City panoramas whose area is covered by real sections are dropped. To fit, the
stream heap gets more block nodes, the wanted / deferred lists grow from 256 / 64 to 1024 entries, and
the draw list grows. → [docs/full_map.md](docs/full_map.md)

**Static batching.** In the loading screen, the instances of the far sections are grouped by 512 m cell,
size class and effect, transformed into world space and merged into static meshes within a memory
limit. Each frame those meshes go into the main view's visibility list as ordinary entries, so the game
draws them with its own effects, textures, blending and fog. Instances with special flags, smokeables
and marker flares are never merged; a texture pack being unloaded drops every mesh that uses it, and
those sections fall back to drawing per instance. Never in the Remix profile:
a merged mesh has a different hash than the objects it was made of. → [docs/static_batching.md](docs/static_batching.md)

**Culling.** Near the camera the sections are walked instance by instance instead of through the
game's cull tree: within the guard radius without frustum test, without occlusion data and without pixel
limit (behind the camera an object's pixel size is 0, which the game would cull), within the occlusion
radius without occlusion data.

**Large address aware.** With more than 2 GB the game's signed pointer compares break; they are turned
into unsigned ones. The game's static D3DX takes pointers above 2 GB for handles, so its allocations
are kept below 2 GB. → [docs/laa_pointer_fix.md](docs/laa_pointer_fix.md)

**The menu.** [Dear ImGui](https://github.com/ocornut/imgui) on the game's device. `Direct3DCreate9` and
`DirectInput8Create` are redirected in the *game's import table*, `CreateDevice` is caught, and
`EndScene` / `Reset` are hooked with [MinHook](https://github.com/TsudaKageyu/minhook). The DX9 backend
is patched to draw pre-transformed vertices, so RTX Remix sees the menu as UI.

**The patch register.** Every byte the plugin writes is registered with its address, the original
bytes, the replacement and an owner, written only over the original bytes and switchable at run time.
The full list is in [docs/addresses.md](docs/addresses.md) and on the *Diagnostics* tab.

### Source layout

| File | |
|---|---|
| `src/core.cpp`, `core.h` | Entry point, executable check, log, the interface between all modules. Start here. |
| `src/patch.cpp` | The patch register. |
| `src/config.cpp` | INI, profiles. |
| `src/lod.cpp` | LOD A for world and cars, pixel limits. |
| `src/fullmap.cpp` | Full map: streaming, section drawing, panoramas, guard radius, world flares. |
| `src/fullmap_tables.h`, `fullmap_replace.h` | Per-region rule tables, generated offline from the game's track data. |
| `src/batch.cpp` | Static batching. |
| `src/despawn.cpp` | Traffic / encounter despawn and flare distance. |
| `src/reflections.cpp` | Reflection render target sizes. |
| `src/pools.cpp` | Memory pools, streamer limits, LAA pointer fix. |
| `src/memfix.cpp` | Crash fixes of the game. |
| `src/settings.cpp` | Forced engine settings. |
| `src/perf.cpp` | Frame time measurement. |
| `src/diag.cpp` | Crash log, hang log, RTX Remix detection and setup check. |
| `src/ui.cpp` | Device and input hooks, the menu. |
| `src/inputguard.cpp`, `inputguard.h` | Holds back other input plugins while the menu is open. |
| `docs/` | Reverse-engineering notes for `SPEED2.EXE` v1.2. |
| `remix_examples/` | `dxvk.conf` and `rtx.conf` for RTX Remix. |
| `third_party/` | Dear ImGui (DX9 backend patched for pre-transformed vertices) and MinHook. |

## Building

Visual Studio 2022 or newer with the *Desktop development with C++* workload. The game is 32-bit, so
the plugin has to be as well.

```bash
msbuild NFSU2_ForceLOD2.vcxproj -p:Configuration=Release -p:Platform=Win32
```

The result is `Release\NFSU2_ForceLOD2.asi`. The project file names the `v145` toolset; if your Visual
Studio is older, change `PlatformToolset` to what you have (`v143` for VS 2022). Everything it needs is
in the repository.

## Antivirus false positives

Some scanners flag the `.asi` – for example Cynet with `Real Protect-LS!…`, or Windows Defender with
names ending in `!ml`. These are false positives, and it is worth understanding why they happen to this
kind of tool.

`Real Protect` and `!ml` verdicts come from behaviour and machine-learning heuristics, not from a
signature of a known malware family. What they react to is the technique, not the intent: a plugin like
this redirects entries in the game's import table, allocates executable memory, writes jump
instructions into loaded code and reads the keyboard. That is what a function hook *is* – and it is also
what a code injector does. Every ASI plugin, ReShade and most trainers get flagged for it sooner or
later. On top of that the file is unsigned and has been downloaded by a handful of people, so it has no
reputation to balance the score. Two builds of identical source can even land on opposite sides,
because the heuristics work on a fuzzy match of the binary.

What you can check for yourself:

- The full source is in this repository – build it yourself and you get the same tool.
- The plugin imports no networking DLLs. Its imports are `KERNEL32`, `USER32`, `GDI32`, `IMM32`,
  `dwmapi`, `SHELL32` (Dear ImGui's Win32 backend) and `dbghelp` (the crash minidump). No `WININET`, no
  `WS2_32`, no `WINHTTP`, no `ADVAPI32` – it cannot phone home, touch the registry or start processes.
- It writes only to its own folder: the INI, the log and, after a crash, a minidump.
- It never injects into any other process. Everything it patches is inside the game that loaded it.

### "Keylogger" / keyboard capture

Some engines and sandbox reports go further and call the plugin a keylogger. The plugin really does
touch the keyboard, with the APIs a keylogger uses. Here is every one of them and why:

| What it does | Why |
|---|---|
| Calls `GetAsyncKeyState` once per frame | To notice the menu key when the DirectInput route below is not available. |
| Hooks `IDirectInputDevice8::GetDeviceState` / `GetDeviceData` | This is how the game reads the keyboard. The hooks look at **one** scan code – the menu key – and, while the menu is open, blank the game's input so typing in the menu does not steer the car. |
| Replaces `GetAsyncKeyState` in the import table of `NFSU_XtendedInput` (and the modules in `InputGuardModules`) | XtendedInput reads the keyboard itself. While the menu is open it is told no key is pressed; when the menu closes it gets the real answer again. |
| Subclasses the game window and forwards messages to Dear ImGui | So the menu's sliders and checkboxes work at all. |

- **Nothing is stored.** There is no keystroke buffer; the hooks keep a single `bool` – "is the menu key
  down right now".
- **Nothing is written.** The log contains hook status, game states and frame times, never a key. Set
  `Log=0` and it writes nothing at all.
- **Nothing can be sent.** No networking library is imported.
- **It only runs inside the game.** No `SetWindowsHookEx`, no driver, no service, no autostart, no
  registry entry. Close the game and nothing of it remains.

If your scanner removes the file, report it as a false positive to the vendor (for Windows Defender:
[Microsoft's submission page](https://www.microsoft.com/en-us/wdsi/filesubmission)) – that fixes it for
everyone. Otherwise add the game's `scripts\` folder to your exclusions, and only do that if you got the
file from this repository's releases.

## Troubleshooting

Look at `scripts\NFSU2_ForceLOD2.log` first. It lists every patch that was applied or refused, the game
states, the streamer, a `[perf]` line every 10 s while driving and a `[perf SPIKE]` line for every frame
over 50 ms. After a crash `NFSU2_ForceLOD2_crash.dmp` lies next to it – please attach both to an issue.

- **Insert does nothing** – check that the ASI loader works (does the log exist?) and that no other
  plugin uses the key. Change `MenuKey` in the INI.
- **`SPEED2.EXE is not the expected v1.2 image - nothing patched`** – your executable is a different
  build than the one this was made for.
- **A patch shows *FOREIGN*** – another plugin owns that address. ForceLOD 2 leaves it alone; the feature
  that needs it is off. The tooltip names the patch.
- **Red counters on the *Status* tab under Remix** – an object was drawn with a different mesh than
  LOD A or dropped. "Draw list full" → raise `DrawListPercent`.
- **Low frame rate in Vanilla** – check that *Static batching* is on (*Map* tab, statistics below it).
  *Diagnostics* shows where the time goes: a large *cull* or *render* share means too many objects –
  raise *Min pixel size* or switch on *Far sections with precull*.
- **Out-of-memory crash after a long session** – install the 4GB patch.
- **Loading screen takes long** – it waits until the whole region is in; with `HangWatchdog` the log
  shows what the streamer is waiting for.

## Known limits

- Without the 4GB patch the largest free block of address space shrinks while driving the full map.
- Terrain and ocean panoramas stay; only city panoramas with real geometry behind them are replaced.
- Pool sizes and reflection sizes need a restart, batching settings the next load.
- Rule tables are matched by region name: a mod that rebuilds one of the original regions under its old
  name gets the original tables.

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT)
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-Clause)
- The NFS modding community – in particular nfsu2-re, Extra Options, Binary and the Unlimiter – and the
  RTX Remix projects of Ekozmaster and adamplayer, whose configurations the Remix setup builds on.

Not affiliated with or endorsed by Electronic Arts. Need for Speed is a trademark of Electronic Arts Inc.
You need your own copy of the game; this repository contains no game files.
