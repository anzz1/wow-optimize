#pragma once
// ================================================================
//  Combat Log Buffer Optimizer for wow_optimize.dll
//  WoW 3.3.5a build 12340
//
//  Prevents combat log data loss in 25-man raids by:
//    1. Increasing retention time (300 -> 1800 sec)
//    2. Periodic CombatLogClearEntries from C level
// ================================================================

#ifndef COMBATLOG_OPTIMIZE_H
#define COMBATLOG_OPTIMIZE_H

#include <windows.h>
#include <cstdint>

namespace CombatLogOpt {

bool Init();

void OnFrame(DWORD mainThreadId);

void Shutdown();

} // namespace CombatLogOpt

#endif