// Patch register: the only place that writes into SPEED2.EXE.
//
// Every site is registered with its original bytes and our replacement. A patch is written only over
// the original bytes; if another plugin already wrote the identical bytes we mark the site "shared"
// and never revert it; anything else is "foreign" and left alone. That keeps us compatible with
// Unlimiter / ExtraOptions / HDReflections without knowing their file names.
#include "core.h"
#include <cstring>
#include <cstdio>
#include "MinHook.h"

namespace {
Patch g_patches[96];
int   g_count = 0;

int Classify(const Patch& p, bool weWroteIt)
{
    const void* cur = reinterpret_cast<const void*>(p.addr);
    if (memcmp(p.orig, p.repl, p.len) == 0) return memcmp(cur, p.orig, p.len) == 0 ? PS_Off : PS_Foreign;   // value = vanilla: nothing to apply
    if (memcmp(cur, p.repl, p.len) == 0) return weWroteIt ? PS_On : PS_Shared;
    if (memcmp(cur, p.orig, p.len) == 0) return PS_Off;
    return PS_Foreign;
}
} // namespace

bool WriteCode(DWORD addr, const void* src, int len)
{
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(addr), src, len);
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
    return true;
}

namespace { bool g_quietAdd = false; }

int PatchAdd(const char* id, const char* module, DWORD addr, const void* orig, const void* repl, int len, const char* note)
{
    if (g_count >= static_cast<int>(sizeof(g_patches) / sizeof(g_patches[0])) || len <= 0 || len > 16) {
        Log("[FAIL] patch register: cannot add %s", id);
        return -1;
    }
    Patch& p = g_patches[g_count];
    memset(&p, 0, sizeof(p));
    strncpy(p.id, id, sizeof(p.id) - 1);
    strncpy(p.module, module, sizeof(p.module) - 1);
    if (note) strncpy(p.note, note, sizeof(p.note) - 1);
    p.addr = addr; p.len = len;
    memcpy(p.orig, orig, len);
    memcpy(p.repl, repl, len);
    p.state = Classify(p, false);
    if (g_quietAdd) return g_count++;
    if (p.state == PS_Foreign) {
        char cur[64] = "", *w = cur;
        for (int i = 0; i < len && i < 12; ++i) w += sprintf(w, "%02X ", reinterpret_cast<const BYTE*>(addr)[i]);
        Log("[FOREIGN] %s @ 0x%08X: bytes %sare neither original nor ours - another plugin owns this site, left alone", id, addr, cur);
    } else if (p.state == PS_Shared) {
        Log("[SHARED] %s @ 0x%08X: another plugin already applied the identical patch", id, addr);
    }
    return g_count++;
}

int PatchAddJmp(const char* id, const char* module, DWORD addr, const void* orig, int len, const void* target, const char* note)
{
    BYTE repl[16];
    repl[0] = 0xE9;
    *reinterpret_cast<DWORD*>(repl + 1) = reinterpret_cast<DWORD>(target) - (addr + 5);
    memset(repl + 5, 0x90, len > 5 ? len - 5 : 0);
    return PatchAdd(id, module, addr, orig, repl, len, note);
}

int PatchAddHook(const char* id, const char* module, DWORD addr, const void* orig, int len, void* detour, void** trampoline, const char* note)
{
    // MinHook writes a 5-byte JMP; the register keeps the first `len` original bytes so the menu can tell
    // "ours" from "someone else's". Switching off = MinHook's own disable (writes the original bytes back).
    if (memcmp(reinterpret_cast<const void*>(addr), orig, len) != 0) {
        int h = PatchAdd(id, module, addr, orig, orig, len, note);   // classified foreign (or off if identical)
        if (h >= 0 && g_patches[h].state != PS_Foreign) g_patches[h].state = PS_Foreign;
        Log("[FOREIGN] %s @ 0x%08X: entry bytes are not the game's - another plugin hooks it, left alone", id, addr);
        return h;
    }
    MH_STATUS st = MH_Initialize();
    if ((st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(reinterpret_cast<void*>(addr), detour, trampoline) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
        Log("[FAIL] %s @ 0x%08X: MinHook", id, addr);
        return -1;
    }
    BYTE now[16]; memcpy(now, reinterpret_cast<const void*>(addr), len);
    g_quietAdd = true;
    int h = PatchAdd(id, module, addr, orig, now, len, note);
    g_quietAdd = false;
    if (h >= 0) { g_patches[h].state = PS_On; g_patches[h].hook = true; Log("[ON] %s @ 0x%08X (hook)", id, addr); }
    return h;
}

bool PatchSet(int h, bool on)
{
    if (h < 0 || h >= g_count) return false;
    Patch& p = g_patches[h];
    if (p.hook) {
        if (p.state != PS_On && p.state != PS_Off) return false;
        if ((p.state == PS_On) == on) return true;
        MH_STATUS st = on ? MH_EnableHook(reinterpret_cast<void*>(p.addr)) : MH_DisableHook(reinterpret_cast<void*>(p.addr));
        if (st != MH_OK) return false;
        p.state = on ? PS_On : PS_Off;
        Log("[%s] %s @ 0x%08X (hook)", on ? "ON" : "OFF", p.id, p.addr);
        return true;
    }
    p.state = Classify(p, p.state == PS_On);
    if (on) {
        if (p.state == PS_On || p.state == PS_Shared) return true;
        if (p.state != PS_Off) return false;
        if (!WriteCode(p.addr, p.repl, p.len)) { p.state = PS_Failed; Log("[FAIL] %s @ 0x%08X: VirtualProtect", p.id, p.addr); return false; }
        p.state = PS_On;
        Log("[ON] %s @ 0x%08X", p.id, p.addr);
        return true;
    }
    if (p.state != PS_On) return p.state == PS_Off;   // shared / foreign stay as they are
    if (!WriteCode(p.addr, p.orig, p.len)) { p.state = PS_Failed; return false; }
    p.state = PS_Off;
    Log("[OFF] %s @ 0x%08X (original restored)", p.id, p.addr);
    return true;
}

bool PatchSetRepl(int h, const void* repl)
{
    if (h < 0 || h >= g_count) return false;
    Patch& p = g_patches[h];
    if (memcmp(p.repl, repl, p.len) == 0) return true;
    bool wasOn = p.state == PS_On;
    if (wasOn && memcmp(reinterpret_cast<const void*>(p.addr), p.repl, p.len) != 0) { p.state = PS_Foreign; return false; }
    memcpy(p.repl, repl, p.len);
    if (wasOn) return WriteCode(p.addr, p.repl, p.len);
    p.state = Classify(p, false);
    return true;
}

bool PatchIsOn(int h)
{
    if (h < 0 || h >= g_count) return false;
    return g_patches[h].state == PS_On || g_patches[h].state == PS_Shared;
}

int PatchCount() { return g_count; }
const Patch* PatchAt(int i) { return (i >= 0 && i < g_count) ? &g_patches[i] : nullptr; }
