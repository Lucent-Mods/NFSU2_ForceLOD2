// InputGuard - stops the game's controls reaching the game while one of our overlays has the input.
//
// Shared between NFSU2_Script, NFSU2_ForceLOD, NFSU2_LiveTextures and NFSU2_ShaderLoader. The file
// is self-contained on purpose: copy inputguard.h and inputguard.cpp into a plugin, call Install()
// once and Tick(blocked) every frame. See inputguard.cpp for what it does and why.
#pragma once
#include <windows.h>

namespace InputGuard {

// Once, after the plugin is up (the game window need not exist yet). Idempotent.
// `owner` is this plugin's own module handle - it identifies our slot in the shared state and keeps
// us from guarding ourselves. `moduleList` is a comma separated list of module names to guard,
// or nullptr for the default ("NFSU_XtendedInput").
void Install(HMODULE owner, const char* moduleList = nullptr);

// Every frame: true while this plugin wants the game to see no input. Also the heartbeat - a
// plugin that stops calling this (crashed, unloaded) releases its block after half a second.
void Tick(bool blocked);

// True while any of our plugins is blocking, this one included.
bool Blocked();

// One line for a debug window or the log: what is guarded and who is blocking.
const char* Status();

// Buffered DirectInput data (IDirectInputDevice8::GetDeviceData) for a device the game must not
// hear while we have the input. Call it after the real GetDeviceData, with the buffer it filled.
//
// Dropping the events is not enough: DirectInput only reports CHANGES, so a key that was already
// held when the block started would stay "down" in the game for as long as the block lasts - open
// the console while accelerating and the car keeps going. So the first blocked call reports
// everything still held as released, exactly once, and after that nothing at all.
//
//   entries   the buffer the device filled (DIDEVICEOBJECTDATA[])
//   stride    cbObjectData, as passed to GetDeviceData
//   capacity  what *pdwInOut was BEFORE the call - how many entries fit
//   count     what *pdwInOut is after it - how many the device reported
//   device    the device pointer, so each device is tracked on its own
// Returns what the caller should report back as *pdwInOut.
unsigned FilterBufferedData(void* entries, unsigned stride, unsigned capacity, unsigned count,
                            bool blocked, const void* device);

} // namespace InputGuard
