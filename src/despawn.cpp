// Later despawns and flares (plan phase 4, docs CULLING_INVENTORY §T/§L/§D of the old ForceLOD).
//   Traffic teleport: TrafficTeleporter_IsWithin250mOfPlayer 0x409800 - `fcomp [0x78429C]` at 0x40981C. A traffic car
//     farther than this (path distance) from the nearest player may be teleported ahead = vanishes.
//   Encounter cars: RandomEncounterManager_DeactivateFarCars 0x40BFB0 - `fcomp [0x78429C]` at 0x40BFF3 -> Car::Deactivate.
//   Both read the shared 250.0 at 0x78429C, which has 4 more readers (encounter spawn, 0x470030): never written - only
//   these two operands are pointed at our own float.
//   Light flares: eRenderLightFlare 0x490920 fades main-view / world flares between 32000 and 40000 (squared distance,
//   179 -> 200 m; `fld [0x78D798]` 0x490F61, `fld [0x78D79C]` 0x490F6A, single readers) -> our own pair, same ratio.
// Live, both profiles (plan decision 17.10.: 400 m).
#include "core.h"

namespace {
float g_trafficDist = 250.0f, g_flareStart2 = 32000.0f, g_flareEnd2 = 40000.0f;
int g_hTraffic = -1, g_hEncounter = -1, g_hFlareStart = -1, g_hFlareEnd = -1;
} // namespace

void DespawnApplyLive()
{
    g_trafficDist = static_cast<float>(g_cfg.trafficDespawn);
    const bool traffic = g_cfg.trafficDespawn != 250;
    PatchSet(g_hTraffic, traffic);
    PatchSet(g_hEncounter, traffic);
    const float end = static_cast<float>(g_cfg.flareDistance);
    const float start = end * 0.8944f;                         // vanilla 178.9 / 200
    g_flareEnd2 = end * end; g_flareStart2 = start * start;
    const bool flares = g_cfg.flareDistance != 200;
    PatchSet(g_hFlareStart, flares);
    PatchSet(g_hFlareEnd, flares);
}

void DespawnInstall()
{
    const DWORD shared = 0x0078429C;
    const DWORD ours = reinterpret_cast<DWORD>(&g_trafficDist);
    g_hTraffic   = PatchAdd("traffic despawn distance", "despawn", 0x0040981E, &shared, &ours, 4, "TrafficTeleporter: 250 m -> own value");
    g_hEncounter = PatchAdd("encounter despawn distance", "despawn", 0x0040BFF5, &shared, &ours, 4, "DeactivateFarCars: 250 m -> own value");
    const DWORD fs = 0x0078D798, fe = 0x0078D79C;
    const DWORD os = reinterpret_cast<DWORD>(&g_flareStart2), oe = reinterpret_cast<DWORD>(&g_flareEnd2);
    g_hFlareStart = PatchAdd("flare fade start", "despawn", 0x00490F63, &fs, &os, 4, "eRenderLightFlare: fade start (squared distance)");
    g_hFlareEnd   = PatchAdd("flare fade end", "despawn", 0x00490F6C, &fe, &oe, 4, "eRenderLightFlare: gone beyond (squared distance)");
    DespawnApplyLive();
    Log("despawn: traffic / encounter %d m, flares up to %d m", g_cfg.trafficDespawn, g_cfg.flareDistance);
}
