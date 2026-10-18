// INI: NFSU2_ForceLOD2.ini next to the .asi.
//
// [General] Profile picks the start values (Remix or Vanilla); every other key overrides one value.
// A missing key means "profile value", so a fresh INI with only Profile= is a complete setup.
#include "core.h"
#include <cstdio>
#include <cstring>

Config g_cfg;

// Hidden engine settings: registry-backed globals the renderer reads every frame.
// CarLighting (0x870CC8) and CarDamage (0x870CCC) are not listed: the renderer never reads them.
const HiddenSettingDef kHidden[HS_Count] = {
    { "CarShadow",       0x00870CC0, 2, "Car drop shadows (decals drawn every frame: dynamic geometry for Remix)" },
    { "CarHeadlight",    0x00870CC4, 1, "Headlight projector pass (view 9) and the headlight effect on scenery in the beam" },
    { "Crowd",           0x00870CD0, 1, "Crowd sprites" },
    { "Fog",             0x00870CD8, 1, "Distance fog" },
    { "MotionBlur",      0x00870CDC, 1, "Motion blur post effect" },
    { "LightStreaks",    0x00870CE0, 1, "Light streak post effect" },
    { "LightGlow",       0x00870CE4, 1, "Light glow sprites (street lights, headlights)" },
    { "AnimatedTexture", 0x00870CE8, 1, "Animated textures (screens, water)" },
    { "ParticleSystem",  0x00870CEC, 1, "Particles (smoke, sparks, rain splashes)" },
    { "DepthOfField",    0x00870CF0, 1, "Depth of field post effect" },
    { "OverBright",      0x00870CFC, 1, "Overbright / bloom" },
    { "BleachByPass",    0x00870D00, 1, "Bleach bypass colour grading" },
    { "Tinting",         0x00870D04, 1, "Screen tinting" },
    { "HorizonFog",      0x00870D0C, 1, "Horizon fog quad" },
    { "Rain",            0x00870D10, 1, "Rain effect" },
};

const char* ProfileName(int p) { return p == PROFILE_VANILLA ? "Vanilla" : "Remix"; }

void ConfigDefaults(Config& c, int profile)
{
    Config fresh;
    char guard[256]; memcpy(guard, c.inputGuardModules, sizeof(guard));
    int key = c.menuKey, lg = c.log;
    c = fresh;
    memcpy(c.inputGuardModules, guard, sizeof(guard));
    c.menuKey = key; c.log = lg;
    c.profile = profile;
    if (profile == PROFILE_REMIX) {
        // Remix: one mesh per object (LOD A everywhere), no extra views that render the scene again,
        // no geometry that is rebuilt every frame (decals, post quads), no headlight effect swap.
        c.hidden[HS_CarShadow] = 0; c.hidden[HS_CarHeadlight] = 0; c.hidden[HS_MotionBlur] = 0;
        c.hidden[HS_LightStreaks] = 0; c.hidden[HS_DepthOfField] = 0; c.hidden[HS_OverBright] = 0;
        c.hidden[HS_BleachByPass] = 0; c.hidden[HS_Tinting] = 0; c.hidden[HS_HorizonFog] = 0;
        c.guardRadius = 100;    // Remix: nothing near the camera disappears (lights / shadows from beside and behind)
        c.precullOffRadius = 200;
        c.drawListPct = 3000;   // capture: every instance of the region in one frame (L4RA ~48 000)
        c.polyBufPct  = 2000;   // capture: one matrix per instance per frame
    } else {
        c.staticBatching = 1;   // merged meshes have other geometry hashes - never for Remix
    }
}

namespace {
char g_ini[MAX_PATH];
int  ReadInt(const char* sec, const char* key, int def) { return GetPrivateProfileIntA(sec, key, def, g_ini); }
void WriteInt(const char* sec, const char* key, int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); WritePrivateProfileStringA(sec, key, b, g_ini); }
int  Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Engine settings are kept per profile ([Settings.Remix] / [Settings.Vanilla]): a value forced in Vanilla (motion blur,
// streaks, DOF ...) must never carry over into Remix - eDirectionalBlur crashed under RTX Remix in the front end
// (2026-10-17, MotionBlur=1 from a Vanilla session). The old shared [Settings] is still read for Vanilla only.
const char* SettingsSection(int profile) { return profile == PROFILE_VANILLA ? "Settings.Vanilla" : "Settings.Remix"; }
void LoadSettings()
{
    for (int i = 0; i < HS_Count; ++i) {
        int v = g_cfg.hidden[i];
        if (g_cfg.profile == PROFILE_VANILLA) v = ReadInt("Settings", kHidden[i].key, v);
        g_cfg.hidden[i] = Clamp(ReadInt(SettingsSection(g_cfg.profile), kHidden[i].key, v), -1, kHidden[i].maxValue);
    }
}

struct IntKey { const char* sec; const char* key; int Config::* field; int lo, hi; };
const IntKey kKeys[] = {
    { "LOD", "WorldMinPixelSize",   &Config::worldMinPx,     1, 64 },
    { "LOD", "WorldPixelHysteresis",&Config::worldPxHyst,    0, 16 },
    { "LOD", "ReflectionLodA",      &Config::reflectionLodA, 0, 1 },
    { "LOD", "CarLodOverride",      &Config::carLod,        -1, 3 },
    { "LOD", "CarTyreLodOverride",  &Config::carTyreLod,    -1, 3 },
    { "LOD", "CullableCarParts",    &Config::cullableParts,  0, 1 },
    { "LOD", "CarMinPixelSize",     &Config::carMinPx,       1, 127 },
    { "LOD", "CarLowDetailPixels",  &Config::carLowDetailPx, 0, 512 },
    { "LOD", "CarBlinkOff",         &Config::carBlinkOff,    0, 1 },
    { "LOD", "PrecullBooBooOff",    &Config::boobooOff,      0, 1 },
    { "LOD", "KeepLodAUnits",       &Config::lodaUnits,      0, 1 },
    { "Culling", "GuardRadius",      &Config::guardRadius,   0, 500 },
    { "Culling", "PrecullOffRadius", &Config::precullOffRadius, 0, 1000 },
    { "FullMap", "Enable",          &Config::fullMap,        0, 1 },
    { "FullMap", "DrawExtraSections", &Config::fullMapExtras, 0, 1 },
    { "FullMap", "ExtraPrecull",     &Config::extraPrecull,   0, 1 },
    { "FullMap", "RemixCaptureAll",  &Config::captureAll,     0, 1 },
    { "FullMap", "ReplacePanoramas", &Config::fullMapReplace, 0, 1 },
    { "FullMap", "ExtraMinPixelSize", &Config::extraMinPx,   0, 64 },
    { "FullMap", "StaticBatching",   &Config::staticBatching, 0, 1 },
    { "FullMap", "BatchTrees",       &Config::batchTrees,    0, 1 },
    { "FullMap", "BatchBudgetMB",    &Config::batchBudgetMB, 16, 512 },
    { "Despawn", "TrafficDistance",  &Config::trafficDespawn, 100, 1000 },
    { "Despawn", "FlareDistance",    &Config::flareDistance,  100, 1000 },
    { "Reflections", "RoadReflectionScale", &Config::roadReflScale, 1, 4 },
    { "Reflections", "CarEnvMapSize",       &Config::carEnvMapSize, 0, 512 },
    { "Reflections", "MirrorSize",          &Config::mirrorSize,    0, 1024 },
    { "Pools", "StreamingPoolPercent", &Config::streamPoolPct, 100, 200 },
    { "Pools", "PackPoolPercent",      &Config::packPoolPct,   100, 400 },
    { "Pools", "PolyBufferPercent",    &Config::polyBufPct,    100, 2000 },
    { "Pools", "FrontEndPoolPercent",  &Config::fengPoolPct,   100, 400 },
    { "Pools", "DrawListPercent",      &Config::drawListPct,   100, 3000 },
    { "Pools", "LaaPointerFix",        &Config::laaFix,        0, 1 },
    { "Pools", "StreamerLimits",       &Config::streamerLimits, 0, 1 },
    { "Memory", "HeapGuard",           &Config::heapGuard,     0, 1 },
    { "Memory", "PoolLock",            &Config::poolLock,      0, 1 },
    { "Memory", "FileGuard",           &Config::fileGuard,     0, 1 },
    { "Memory", "LowMemD3DX",          &Config::lowMemD3dx,    0, 1 },
    { "General", "MenuKey",       &Config::menuKey,      1, 254 },
    { "General", "Log",           &Config::log,          0, 1 },
    { "General", "CrashLog",      &Config::crashLog,     0, 1 },
    { "General", "HangWatchdog",  &Config::hangWatchdog, 0, 600 },
    { "General", "InputBlock",    &Config::inputBlock,   0, 1 },
};
} // namespace

// menu profile switch: profile values, then that profile's own engine settings from the INI
void ConfigSwitchProfile(int profile)
{
    ConfigDefaults(g_cfg, profile);
    if (g_ini[0]) LoadSettings();
}

void ConfigLoad()
{
    snprintf(g_ini, sizeof(g_ini), "%s\\NFSU2_ForceLOD2.ini", g_baseDir);
    char prof[32];
    GetPrivateProfileStringA("General", "Profile", "Remix", prof, sizeof(prof), g_ini);
    ConfigDefaults(g_cfg, _stricmp(prof, "Vanilla") == 0 ? PROFILE_VANILLA : PROFILE_REMIX);
    for (const IntKey& k : kKeys) g_cfg.*k.field = Clamp(ReadInt(k.sec, k.key, g_cfg.*k.field), k.lo, k.hi);
    if (g_cfg.roadReflScale != 2 && g_cfg.roadReflScale != 4) g_cfg.roadReflScale = 1;        // only the sizes the old tool offered
    if (g_cfg.carEnvMapSize != 256 && g_cfg.carEnvMapSize != 512) g_cfg.carEnvMapSize = 0;
    if (g_cfg.mirrorSize != 512 && g_cfg.mirrorSize != 1024) g_cfg.mirrorSize = 0;
    LoadSettings();
    char mods[256];
    GetPrivateProfileStringA("General", "InputGuardModules", g_cfg.inputGuardModules, mods, sizeof(mods), g_ini);
    memcpy(g_cfg.inputGuardModules, mods, sizeof(mods));
}

void ConfigSave()
{
    // only what differs from the profile is written; a key equal to the profile value is removed, so switching
    // Profile= in the file still switches everything that was never changed by hand
    Config def; ConfigDefaults(def, g_cfg.profile);
    WritePrivateProfileStringA("General", "Profile", ProfileName(g_cfg.profile), g_ini);
    for (const IntKey& k : kKeys) {
        if (g_cfg.*k.field == def.*k.field) WritePrivateProfileStringA(k.sec, k.key, nullptr, g_ini);
        else WriteInt(k.sec, k.key, g_cfg.*k.field);
    }
    const char* sec = SettingsSection(g_cfg.profile);
    for (int i = 0; i < HS_Count; ++i) {
        if (g_cfg.hidden[i] == def.hidden[i]) WritePrivateProfileStringA(sec, kHidden[i].key, nullptr, g_ini);
        else WriteInt(sec, kHidden[i].key, g_cfg.hidden[i]);
    }
    if (g_cfg.profile == PROFILE_VANILLA) WritePrivateProfileStringA("Settings", nullptr, nullptr, g_ini);   // old shared section: now in Settings.Vanilla
    WritePrivateProfileStringA("General", "InputGuardModules", g_cfg.inputGuardModules, g_ini);
    Log("INI saved (%s)", g_ini);
}
