#pragma once
// ================================================================
//  Combat Log Buffer Optimizer for wow_optimize.dll
//  WoW 3.3.5a build 12340
//
//  Prevents combat log data loss in 25-man raids by:
//    1. Increasing retention time (300 -> 900 sec)
//    2. Disabling retention-based entry recycling
//    3. Periodic cleanup of expired entries from C level
// ================================================================

#ifndef COMBATLOG_OPTIMIZE_H
#define COMBATLOG_OPTIMIZE_H

#include <windows.h>
#include <cstdint>

namespace CombatLogOpt {

// Call from MainThread after game init (5+ sec delay).
bool Init();

// Call from hooked_Sleep on main thread, every frame.
void OnFrame(DWORD mainThreadId);

// Call on DLL unload.
void Shutdown();

} // namespace CombatLogOpt

#endif