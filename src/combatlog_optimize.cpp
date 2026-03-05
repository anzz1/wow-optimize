// ================================================================
//  Combat Log Buffer Optimizer — Implementation
//  WoW 3.3.5a build 12340 (Warmane)
//
//  Prevents combat log data loss in raids by increasing the
//  retention time for combat log entries. Combined with periodic
//  cleanup of expired entries, this ensures addons have enough
//  time to process all events without the allocator recycling
//  unprocessed entries.
//
//  Architecture:
//    ADB97C = HEAD of active entry list (oldest first)
//    ADB988 = HEAD of free list (recycled entries)
//    CA1394 = pointer to first unprocessed entry (Lua push queue)
//    BD09F0 = CVar* "combatLogRetentionTime" (default "300")
//    CD76AC = game clock in milliseconds
//
//  v1.4.1: Removed the js→jmp recycle patch. It caused crashes
//  on teleport (entries accumulated indefinitely) and broke
//  WeakAuras in battlegrounds (event queue grew unbounded).
//  Retention increase + periodic cleanup is sufficient and safe.
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
    static constexpr uintptr_t CVar_RetentionPtr  = 0x00BD09F0;
    static constexpr uintptr_t ActiveListHead     = 0x00ADB97C;
    static constexpr uintptr_t PendingEntry       = 0x00CA1394;
    static constexpr uintptr_t CurrentTime        = 0x00CD76AC;
    static constexpr uintptr_t FreeEntryFunc      = 0x00750390;
}

static constexpr int CVAR_INT_OFFSET = 0x30;

// ================================================================
//  Configuration
// ================================================================

static constexpr int TARGET_RETENTION_SEC     = 1800;  // 30 min (default 300 = 5 min)
static constexpr int CLEANUP_INTERVAL_FRAMES  = 600;   // ~10 sec at 60fps
static constexpr int RETENTION_CHECK_FRAMES   = 600;
static constexpr int MAX_FREE_PER_CLEANUP     = 50000;
static constexpr int MAX_RETENTION_RETRIES    = 600;

// ================================================================
//  State
// ================================================================

static bool    g_initialized       = false;
static int     g_origRetention     = 0;
static bool    g_retentionPatched  = false;
static bool    g_retentionGaveUp   = false;
static int     g_retentionRetries  = 0;

static int g_cleanupCounter        = 0;
static int g_retentionCheckCounter = 0;
static int g_totalEntriesCleaned   = 0;
static int g_cleanupCycles         = 0;

typedef void (__fastcall *FreeEntry_fn)(void* entry, void* edx_unused);
static FreeEntry_fn g_freeEntry = nullptr;

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
//  Periodic Cleanup
//
//  Walks from HEAD (oldest) and frees entries that:
//    1. Have expired (age > retention time)
//    2. Have already been processed by Lua (entry != CA1394)
//
//  This is the safe version of what CombatLogFix addon does,
//  but from C level without Lua overhead.
// ================================================================

static void CleanupExpiredEntries() {
    __try {
        uintptr_t cvarPtr = *(uintptr_t*)Addr::CVar_RetentionPtr;
        if (!cvarPtr) return;

        int retentionSec = *(int*)(cvarPtr + CVAR_INT_OFFSET);
        if (retentionSec <= 0) return;
        int retentionMs = retentionSec * 1000;

        int currentTime = *(int*)Addr::CurrentTime;
        uintptr_t pending = *(uintptr_t*)Addr::PendingEntry;

        int freed = 0;
        uintptr_t prevHead = 0;

        for (int i = 0; i < MAX_FREE_PER_CLEANUP; i++) {
            uintptr_t head = *(uintptr_t*)Addr::ActiveListHead;

            if (!head || (head & 1)) break;
            if (head == prevHead) break;
            if (!IsReadable(head + 8)) break;

            // Never free entries that Lua hasn't processed yet
            if (head == pending) break;

            int timestamp = *(int*)(head + 8);
            int age = currentTime - timestamp;
            if (age < retentionMs) break;

            prevHead = head;
            g_freeEntry((void*)head, nullptr);
            freed++;
        }

        if (freed > 0) {
            g_totalEntriesCleaned += freed;
            g_cleanupCycles++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[CombatLog] Exception in cleanup");
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

    if (!IsExecutable(Addr::FreeEntryFunc)) {
        Log("[CombatLog] FreeEntry (0x%08X) not found — aborting",
            (unsigned)Addr::FreeEntryFunc);
        return false;
    }
    g_freeEntry = (FreeEntry_fn)Addr::FreeEntryFunc;

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

    Log("[CombatLog]  [ OK ] Periodic cleanup (every %d frames)",
        CLEANUP_INTERVAL_FRAMES);
    Log("[CombatLog] ====================================");

    g_initialized = true;
    return g_retentionPatched;
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

    // Re-apply retention if overwritten by addon or /console
    g_retentionCheckCounter++;
    if (g_retentionPatched && g_retentionCheckCounter >= RETENTION_CHECK_FRAMES) {
        g_retentionCheckCounter = 0;
        int current = ReadRetention();
        if (current > 0 && current != TARGET_RETENTION_SEC) {
            WriteRetention(TARGET_RETENTION_SEC);
        }
    }

    // Periodic cleanup of expired & processed entries
    g_cleanupCounter++;
    if (g_cleanupCounter >= CLEANUP_INTERVAL_FRAMES) {
        g_cleanupCounter = 0;
        if (g_freeEntry) {
            CleanupExpiredEntries();
        }
    }
}

void Shutdown() {
    if (!g_initialized) return;

    if (g_retentionPatched) {
        WriteRetention(g_origRetention);
        Log("[CombatLog] Retention restored to %d sec", g_origRetention);
        g_retentionPatched = false;
    }

    Log("[CombatLog] Shutdown. Cleaned %d entries in %d cycles",
        g_totalEntriesCleaned, g_cleanupCycles);

    g_initialized = false;
}

} // namespace CombatLogOpt