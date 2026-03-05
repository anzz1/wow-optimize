// ================================================================
//  Combat Log Buffer Optimizer — Implementation
//  WoW 3.3.5a build 12340 (Warmane)
//
//  Two-layer fix for combat log breaks:
//
//  Layer 1: Retention increase (300 -> 1800 sec)
//    Prevents the allocator from recycling entries that Lua
//    hasn't processed yet. Addresses the root cause of data
//    loss during heavy combat bursts.
//
//  Layer 2: Periodic CombatLogClearEntries
//    Clears all processed entries every ~10 seconds.
//    Addresses secondary causes: internal list corruption,
//    stuck CA1394 pointer, dispatch loop errors.
//    Only runs when CA1394 == NULL (all entries processed).
//    Equivalent to CombatLogFix addon but from C level.
// ================================================================

#include "combatlog_optimize.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" void Log(const char* fmt, ...);

// ================================================================
//  Known Addresses — build 12340
// ================================================================

namespace Addr {
    static constexpr uintptr_t CVar_RetentionPtr     = 0x00BD09F0;
    static constexpr uintptr_t ActiveListHead        = 0x00ADB97C;
    static constexpr uintptr_t PendingEntry          = 0x00CA1394;
    static constexpr uintptr_t CurrentTime           = 0x00CD76AC;
    static constexpr uintptr_t CombatLogClearEntries = 0x00751120;
}

static constexpr int CVAR_INT_OFFSET = 0x30;

// ================================================================
//  Configuration
// ================================================================

static constexpr int TARGET_RETENTION_SEC    = 1800;  // 30 min (default 300)
static constexpr int CLEAR_INTERVAL_FRAMES   = 600;   // ~10 sec at 60fps
static constexpr int RETENTION_CHECK_FRAMES  = 600;
static constexpr int MAX_RETENTION_RETRIES   = 600;

// ================================================================
//  State
// ================================================================

static bool    g_initialized       = false;
static int     g_origRetention     = 0;
static bool    g_retentionPatched  = false;
static bool    g_retentionGaveUp   = false;
static int     g_retentionRetries  = 0;

static int g_clearCounter          = 0;
static int g_retentionCheckCounter = 0;
static int g_totalClears           = 0;

// CombatLogClearEntries is a simple function with no args,
// uses ecx from global (reads ADB97C internally).
typedef int (__cdecl *ClearEntries_fn)();
static ClearEntries_fn g_clearEntries = nullptr;

// ================================================================
//  Memory Validation
// ================================================================

static bool IsReadable(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
}

static bool IsExecutable(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// ================================================================
//  CVar Access
// ================================================================

static int ReadRetention() {
    __try {
        uintptr_t cvarPtr = *(uintptr_t*)Addr::CVar_RetentionPtr;
        if (!cvarPtr || !IsReadable(cvarPtr + CVAR_INT_OFFSET)) return -1;
        return *(int*)(cvarPtr + CVAR_INT_OFFSET);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static bool WriteRetention(int seconds) {
    __try {
        uintptr_t cvarPtr = *(uintptr_t*)Addr::CVar_RetentionPtr;
        if (!cvarPtr || !IsReadable(cvarPtr + CVAR_INT_OFFSET)) return false;

        DWORD oldProtect;
        VirtualProtect((void*)(cvarPtr + CVAR_INT_OFFSET), 4, PAGE_READWRITE, &oldProtect);
        *(int*)(cvarPtr + CVAR_INT_OFFSET) = seconds;
        VirtualProtect((void*)(cvarPtr + CVAR_INT_OFFSET), 4, oldProtect, &oldProtect);

        return (*(int*)(cvarPtr + CVAR_INT_OFFSET) == seconds);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ================================================================
//  Retention Patch (with retry)
// ================================================================

static int TryPatchRetention() {
    if (!IsReadable(Addr::CVar_RetentionPtr)) {
        return 0;
    }

    int current = ReadRetention();
    if (current < 0) {
        return 0;
    }

    if (current <= 0 || current > 100000) {
        Log("[CombatLog] Implausible retention value: %d", current);
        return -1;
    }

    g_origRetention = current;

    if (!WriteRetention(TARGET_RETENTION_SEC)) {
        Log("[CombatLog] Failed to write retention value");
        return -1;
    }

    g_retentionPatched = true;
    return 1;
}

// ================================================================
//  Periodic Clear
//
//  Calls CombatLogClearEntries when all entries have been
//  processed by Lua (CA1394 == NULL). This prevents the combat
//  log from entering a broken state due to internal corruption,
//  stuck pointers, or dispatch errors.
//
//  Equivalent to the CombatLogFix addon but from C level:
//    local f = CreateFrame("Frame")
//    f:SetScript("OnUpdate", CombatLogClearEntries)
//
//  We run it every ~10 seconds instead of every frame — less
//  aggressive but still effective. Events are dispatched to Lua
//  via COMBAT_LOG_EVENT_UNFILTERED before we clear, so addons
//  receive all data.
// ================================================================

static void TryClearProcessedEntries() {
    __try {
        // Only clear when Lua has processed everything
        uintptr_t pending = *(uintptr_t*)Addr::PendingEntry;
        if (pending != 0) return;

        // Only clear if there are entries to clear
        uintptr_t head = *(uintptr_t*)Addr::ActiveListHead;
        if (!head || (head & 1)) return;

        g_clearEntries();
        g_totalClears++;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[CombatLog] Exception in clear");
    }
}

// ================================================================
//  Public API
// ================================================================

namespace CombatLogOpt {

bool Init() {
    Log("[CombatLog] ====================================");
    Log("[CombatLog]  Combat Log Buffer Optimizer");
    Log("[CombatLog]  Build 12340");
    Log("[CombatLog] ====================================");

    if (!IsExecutable(Addr::CombatLogClearEntries)) {
        Log("[CombatLog] CombatLogClearEntries not found — aborting");
        return false;
    }
    g_clearEntries = (ClearEntries_fn)Addr::CombatLogClearEntries;

    int retResult = TryPatchRetention();
    if (retResult == 1) {
        Log("[CombatLog]  [ OK ] Retention time (%d -> %d sec)",
            g_origRetention, TARGET_RETENTION_SEC);
    } else if (retResult == 0) {
        Log("[CombatLog]  [WAIT] Retention time — CVar not ready, will retry");
    } else {
        Log("[CombatLog]  [FAIL] Retention time");
        g_retentionGaveUp = true;
    }

    Log("[CombatLog]  [ OK ] Periodic clear (every %d frames, safe)",
        CLEAR_INTERVAL_FRAMES);
    Log("[CombatLog] ====================================");

    g_initialized = true;
    return true;
}

void OnFrame(DWORD mainThreadId) {
    if (!g_initialized) return;
    if (GetCurrentThreadId() != mainThreadId) return;

    // Retry retention patch if CVar wasn't ready during Init
    if (!g_retentionPatched && !g_retentionGaveUp) {
        g_retentionRetries++;
        if ((g_retentionRetries & 15) == 0) {
            int result = TryPatchRetention();
            if (result == 1) {
                Log("[CombatLog] Retention patched on retry #%d: %d -> %d sec",
                    g_retentionRetries, g_origRetention, TARGET_RETENTION_SEC);
            } else if (result == -1 || g_retentionRetries >= MAX_RETENTION_RETRIES) {
                g_retentionGaveUp = true;
                Log("[CombatLog] Retention patch failed after %d retries",
                    g_retentionRetries);
            }
        }
    }

    // Re-apply retention if overwritten
    g_retentionCheckCounter++;
    if (g_retentionPatched && g_retentionCheckCounter >= RETENTION_CHECK_FRAMES) {
        g_retentionCheckCounter = 0;
        int current = ReadRetention();
        if (current > 0 && current != TARGET_RETENTION_SEC) {
            WriteRetention(TARGET_RETENTION_SEC);
        }
    }

    // Periodic clear of processed entries
    g_clearCounter++;
    if (g_clearCounter >= CLEAR_INTERVAL_FRAMES) {
        g_clearCounter = 0;
        TryClearProcessedEntries();
    }
}

void Shutdown() {
    if (!g_initialized) return;

    if (g_retentionPatched) {
        WriteRetention(g_origRetention);
        Log("[CombatLog] Retention restored to %d sec", g_origRetention);
        g_retentionPatched = false;
    }

    Log("[CombatLog] Shutdown. Total clears: %d", g_totalClears);

    g_initialized = false;
}

} // namespace CombatLogOpt