// ================================================================
//  Combat Log Buffer Optimizer — Implementation
//  WoW 3.3.5a build 12340 (Warmane)
//
//  Combat log uses a doubly-linked list of entries. When the free
//  list is empty and a new entry is needed, the allocator
//  (sub_750400) checks if the oldest entry has expired per the
//  retention CVar. If it has, the entry is recycled — even if Lua
//  hasn't processed it yet (CA1394 pointer). This causes combat
//  log breaks: addons like Skada/Recount miss events.
//
//  Architecture:
//    ADB97C = HEAD of active entry list (oldest first)
//    ADB988 = HEAD of free list (recycled entries)
//    ADB980 = free list manager structure
//    CA1394 = pointer to first unprocessed entry (Lua push queue)
//    CA1390 = tail tracking pointer
//    BD09F0 = CVar* "combatLogRetentionTime" (default "300")
//    CD76AC = game clock in milliseconds
//
//  Entry structure (120+ bytes per node):
//    +0x00  prev pointer
//    +0x04  next pointer
//    +0x08  timestamp (game time ms)
//    +0x10  parent/child flag
//    +0x54  event flags
//    +0x58  event subtype
//
//  sub_750400: entry allocator
//    1. Free list has entries → reuse
//    2. Free list empty, oldest expired → recycle (DATA LOSS!)
//    3. Free list empty, oldest not expired → heap alloc new
//
//  Fix: patch step 2 to always go to step 3 (js → jmp).
//  Expired entries are freed by our periodic cleanup instead,
//  which respects the CA1394 boundary.
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
    static constexpr uintptr_t FreeListHead       = 0x00ADB988;
    static constexpr uintptr_t PendingEntry       = 0x00CA1394;
    static constexpr uintptr_t CurrentTime        = 0x00CD76AC;
    static constexpr uintptr_t FreeEntryFunc      = 0x00750390;
    static constexpr uintptr_t RecycleJumpAddr    = 0x0075043D;
}

static constexpr int CVAR_INT_OFFSET = 0x30;

// ================================================================
//  Configuration
// ================================================================

static constexpr int TARGET_RETENTION_SEC      = 900;
static constexpr int CLEANUP_INTERVAL_FRAMES   = 1800;
static constexpr int RETENTION_CHECK_FRAMES    = 600;
static constexpr int MAX_FREE_PER_CLEANUP      = 50000;
static constexpr int MAX_RETENTION_RETRIES      = 600;   // ~10 sec at 60fps

// ================================================================
//  State
// ================================================================

static bool    g_initialized       = false;
static int     g_origRetention     = 0;
static bool    g_retentionPatched  = false;
static bool    g_retentionGaveUp   = false;
static int     g_retentionRetries  = 0;
static bool    g_recyclePatched    = false;
static uint8_t g_origRecycleByte   = 0;

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
//  Patch 1: Retention Time (with retry support)
// ================================================================

// Returns: 1 = success, 0 = not ready (retry later), -1 = permanent failure
static int TryPatchRetention() {
    if (!IsReadable(Addr::CVar_RetentionPtr)) {
        return 0;
    }

    int current = ReadRetention();
    if (current < 0) {
        return 0;  // CVar not initialized yet
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
//  Patch 2: Disable Retention Recycling
// ================================================================

static bool PatchRecycle() {
    if (!IsExecutable(Addr::RecycleJumpAddr)) {
        Log("[CombatLog] Recycle patch address not executable");
        return false;
    }

    __try {
        uint8_t current = *(uint8_t*)Addr::RecycleJumpAddr;
        if (current != 0x78) {
            Log("[CombatLog] Expected 0x78 (js) at 0x%08X, found 0x%02X",
                (unsigned)Addr::RecycleJumpAddr, current);
            return false;
        }

        g_origRecycleByte = current;

        DWORD oldProtect;
        VirtualProtect((void*)Addr::RecycleJumpAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
        *(uint8_t*)Addr::RecycleJumpAddr = 0xEB;
        VirtualProtect((void*)Addr::RecycleJumpAddr, 1, oldProtect, &oldProtect);

        g_recyclePatched = true;
        Log("[CombatLog] Retention recycling disabled (0x%08X: js -> jmp)",
            (unsigned)Addr::RecycleJumpAddr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[CombatLog] Exception in recycle patch");
        return false;
    }
}

// ================================================================
//  Periodic Cleanup
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
            Log("[CombatLog] Cleanup #%d: freed %d expired entries (total: %d)",
                g_cleanupCycles, freed, g_totalEntriesCleaned);
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

    bool recOk = PatchRecycle();
    Log("[CombatLog]  [%s] Prevent entry recycling",
        recOk ? " OK " : "SKIP");
    Log("[CombatLog]  [ OK ] Periodic cleanup (every %d frames)",
        CLEANUP_INTERVAL_FRAMES);
    Log("[CombatLog] ====================================");

    g_initialized = true;
    return recOk || g_retentionPatched;
}

void OnFrame(DWORD mainThreadId) {
    if (!g_initialized) return;
    if (GetCurrentThreadId() != mainThreadId) return;

    // Retry retention patch if it wasn't ready during Init
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

    // Re-apply retention if something overwrites it
    g_retentionCheckCounter++;
    if (g_retentionPatched && g_retentionCheckCounter >= RETENTION_CHECK_FRAMES) {
        g_retentionCheckCounter = 0;
        int current = ReadRetention();
        if (current > 0 && current != TARGET_RETENTION_SEC) {
            WriteRetention(TARGET_RETENTION_SEC);
        }
    }

    // Cleanup expired entries
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

    if (g_recyclePatched) {
        __try {
            DWORD oldProtect;
            VirtualProtect((void*)Addr::RecycleJumpAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect);
            *(uint8_t*)Addr::RecycleJumpAddr = g_origRecycleByte;
            VirtualProtect((void*)Addr::RecycleJumpAddr, 1, oldProtect, &oldProtect);
            Log("[CombatLog] Recycle patch restored");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[CombatLog] Exception restoring recycle patch");
        }
        g_recyclePatched = false;
    }

    Log("[CombatLog] Shutdown. Cleaned %d entries in %d cycles",
        g_totalEntriesCleaned, g_cleanupCycles);

    g_initialized = false;
}

} // namespace CombatLogOpt