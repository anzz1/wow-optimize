// ================================================================
//  Lua VM Optimizer — Implementation
//  WoW 3.3.5a build 12340 (Warmane)
//
//  4-tier GC stepping: loading → combat → idle → normal
//  Reads addon state from Lua globals, adjusts step size per-frame.
//
//  IMPORTANT: WoW validates C function pointers passed via
//  lua_pushcclosure(). If the pointer is outside Wow.exe's
//  code section, WoW crashes with "Invalid function pointer".
//
//  Therefore we DO NOT register any C functions to Lua.
//  Instead we:
//    1. Call lua_gc() directly for GC stepping (C→Lua API, safe)
//    2. Write stats to Lua globals via lua_push*/lua_setfield
//    3. Use FrameScript_Execute for complex Lua-side setup
//    4. The addon reads our globals — no C callbacks needed
// ================================================================

#include "lua_optimize.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <psapi.h>
#include <mimalloc.h>

extern "C" void Log(const char* fmt, ...);

// ================================================================
//  Lua 5.1 Types
// ================================================================

typedef struct lua_State lua_State;
typedef double lua_Number;

#define LUA_GCSTOP       0
#define LUA_GCRESTART    1
#define LUA_GCCOLLECT    2
#define LUA_GCCOUNT      3
#define LUA_GCCOUNTB     4
#define LUA_GCSTEP       5
#define LUA_GCSETPAUSE   6
#define LUA_GCSETSTEPMUL 7

#define LUA_GLOBALSINDEX (-10002)

// ================================================================
//  Function Pointer Types
// ================================================================

typedef int         (__cdecl *fn_lua_gc)(lua_State* L, int what, int data);
typedef int         (__cdecl *fn_lua_gettop)(lua_State* L);
typedef void        (__cdecl *fn_lua_settop)(lua_State* L, int index);
typedef lua_Number  (__cdecl *fn_lua_tonumber)(lua_State* L, int index);
typedef int         (__cdecl *fn_lua_toboolean)(lua_State* L, int index);
typedef void        (__cdecl *fn_lua_pushnumber)(lua_State* L, lua_Number n);
typedef void        (__cdecl *fn_lua_pushboolean)(lua_State* L, int b);
typedef const char* (__cdecl *fn_lua_pushstring)(lua_State* L, const char* s);
typedef void        (__cdecl *fn_lua_pushnil)(lua_State* L);
typedef void        (__cdecl *fn_lua_setfield)(lua_State* L, int index, const char* k);
typedef void        (__cdecl *fn_lua_getfield)(lua_State* L, int index, const char* k);
typedef int         (__cdecl *fn_lua_type)(lua_State* L, int index);

typedef void        (__cdecl *fn_FrameScript_Execute)(const char* code,
                                                       const char* source,
                                                       int unknown);

// ================================================================
//  Known Addresses — build 12340 (IDA Pro)
// ================================================================

namespace Addr {
    static constexpr uintptr_t lua_State_ptr       = 0x00D3F78C;
    static constexpr uintptr_t FrameScript_Execute = 0x00819210;
    static constexpr uintptr_t lua_gc              = 0x0084ED50;
    static constexpr uintptr_t lua_gettop          = 0x0084DBD0;
    static constexpr uintptr_t lua_settop          = 0x0084DBF0;
    static constexpr uintptr_t lua_pushnumber      = 0x0084E2A0;
    static constexpr uintptr_t lua_pushboolean     = 0x0084E4D0;
    static constexpr uintptr_t lua_pushstring      = 0x0084E350;
    static constexpr uintptr_t lua_pushnil         = 0x0084E280;
    static constexpr uintptr_t lua_setfield        = 0x0084E900;
    static constexpr uintptr_t lua_getfield        = 0x0084E590;
    static constexpr uintptr_t lua_tonumber        = 0x0084E030;
    static constexpr uintptr_t lua_toboolean       = 0x0084E0B0;
    static constexpr uintptr_t lua_type            = 0x0084DEB0;
}

// ================================================================
//  Resolved API
// ================================================================

static struct {
    lua_State*              L = nullptr;

    fn_lua_gc               lua_gc = nullptr;
    fn_lua_gettop           lua_gettop = nullptr;
    fn_lua_settop           lua_settop = nullptr;
    fn_lua_tonumber         lua_tonumber = nullptr;
    fn_lua_toboolean        lua_toboolean = nullptr;
    fn_lua_pushnumber       lua_pushnumber = nullptr;
    fn_lua_pushboolean      lua_pushboolean = nullptr;
    fn_lua_pushstring       lua_pushstring = nullptr;
    fn_lua_pushnil          lua_pushnil = nullptr;
    fn_lua_setfield         lua_setfield = nullptr;
    fn_lua_getfield         lua_getfield = nullptr;
    fn_lua_type             lua_type = nullptr;

    fn_FrameScript_Execute  FrameScript_Execute = nullptr;
} Api;

// ================================================================
//  Configuration — 4-tier GC stepping
//
//  Priority order (highest first):
//    loading  → loadingStepKB  (most aggressive, screen is frozen)
//    combat   → combatStepKB   (minimal, protect frametime)
//    idle     → idleStepKB     (aggressive, player is AFK)
//    normal   → normalStepKB   (balanced)
// ================================================================

static struct {
    int  gcPause        = 110;
    int  gcStepMul      = 300;
    int  normalStepKB   = 64;
    int  combatStepKB   = 16;
    int  idleStepKB     = 128;
    int  loadingStepKB  = 256;
    bool manualGCMode   = true;

    // State read from addon globals every frame
    bool inCombat       = false;
    bool isIdle         = false;
    bool isLoading      = false;
} Config;

// ================================================================
//  Runtime State
// ================================================================

static volatile LONG g_luaInitState = 0;  // 0=idle, 1=pending, 2=done
static volatile bool g_addressesValid = false;

static struct {
    bool   initialized     = false;
    bool   gcOptimized     = false;

    int    origGCPause     = 200;
    int    origGCStepMul   = 200;

    double luaMemoryKB     = 0.0;
    int    gcStepsTotal    = 0;
    int    fullCollects    = 0;

    int    statsUpdateCounter = 0;

    // Track mode changes for logging
    const char* lastModeName = "unknown";
} State;

// ================================================================
//  Memory Validation
// ================================================================

static bool IsExecutableMemory(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static bool IsReadableMemory(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & PAGE_NOACCESS) == 0 &&
           (mbi.Protect & PAGE_GUARD) == 0;
}

// ================================================================
//  Address Resolution
// ================================================================

static bool ResolveAddresses() {
    Log("[LuaOpt] Resolving addresses for build 12340...");

    int found = 0;
    int failed = 0;

    #define RESOLVE(field, addr, type, name)                             \
        if (IsExecutableMemory(addr)) {                                  \
            Api.field = (type)(addr);                                    \
            found++;                                                     \
            Log("[LuaOpt]   %-22s 0x%08X  OK", name, (unsigned)(addr));  \
        } else {                                                         \
            failed++;                                                    \
            Log("[LuaOpt]   %-22s 0x%08X  INVALID", name, (unsigned)(addr)); \
        }

    RESOLVE(lua_gc,          Addr::lua_gc,          fn_lua_gc,          "lua_gc");
    RESOLVE(lua_gettop,      Addr::lua_gettop,      fn_lua_gettop,      "lua_gettop");
    RESOLVE(lua_settop,      Addr::lua_settop,      fn_lua_settop,      "lua_settop");
    RESOLVE(lua_tonumber,    Addr::lua_tonumber,     fn_lua_tonumber,    "lua_tonumber");
    RESOLVE(lua_toboolean,   Addr::lua_toboolean,    fn_lua_toboolean,   "lua_toboolean");
    RESOLVE(lua_pushnumber,  Addr::lua_pushnumber,   fn_lua_pushnumber,  "lua_pushnumber");
    RESOLVE(lua_pushboolean, Addr::lua_pushboolean,  fn_lua_pushboolean, "lua_pushboolean");
    RESOLVE(lua_pushstring,  Addr::lua_pushstring,   fn_lua_pushstring,  "lua_pushstring");
    RESOLVE(lua_pushnil,     Addr::lua_pushnil,      fn_lua_pushnil,     "lua_pushnil");
    RESOLVE(lua_setfield,    Addr::lua_setfield,     fn_lua_setfield,    "lua_setfield");
    RESOLVE(lua_getfield,    Addr::lua_getfield,     fn_lua_getfield,    "lua_getfield");
    RESOLVE(lua_type,        Addr::lua_type,         fn_lua_type,        "lua_type");
    RESOLVE(FrameScript_Execute, Addr::FrameScript_Execute,
            fn_FrameScript_Execute, "FrameScript_Execute");

    #undef RESOLVE

    if (IsReadableMemory(Addr::lua_State_ptr)) {
        Log("[LuaOpt]   %-22s 0x%08X  OK (data)", "lua_State* ptr", (unsigned)Addr::lua_State_ptr);
        found++;
    } else {
        Log("[LuaOpt]   %-22s 0x%08X  INVALID", "lua_State* ptr", (unsigned)Addr::lua_State_ptr);
        failed++;
    }

    Log("[LuaOpt] Resolved: %d OK, %d FAILED", found, failed);

    if (!Api.lua_gc) {
        Log("[LuaOpt] CRITICAL: lua_gc not available");
        return false;
    }

    return true;
}

// ================================================================
//  Get lua_State*
// ================================================================

static lua_State* ReadLuaState() {
    if (!IsReadableMemory(Addr::lua_State_ptr)) return nullptr;

    __try {
        lua_State* L = *(lua_State**)(Addr::lua_State_ptr);
        if ((uintptr_t)L < 0x00010000 || (uintptr_t)L > 0x7FFFFFFF)
            return nullptr;
        if (!IsReadableMemory((uintptr_t)L))
            return nullptr;
        return L;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ================================================================
//  GC Optimization
// ================================================================

static bool OptimizeGC(lua_State* L) {
    if (!Api.lua_gc) return false;

    __try {
        int testMem = Api.lua_gc(L, LUA_GCCOUNT, 0);
        if (testMem < 0 || testMem > 4 * 1024 * 1024) {
            Log("[LuaOpt] lua_gc returned implausible value %d — aborting", testMem);
            return false;
        }
        Log("[LuaOpt] lua_gc verified OK: Lua memory = %d KB", testMem);

        State.origGCPause   = Api.lua_gc(L, LUA_GCSETPAUSE,   Config.gcPause);
        State.origGCStepMul = Api.lua_gc(L, LUA_GCSETSTEPMUL, Config.gcStepMul);

        Log("[LuaOpt] GC tuned: pause %d -> %d, stepmul %d -> %d",
            State.origGCPause, Config.gcPause,
            State.origGCStepMul, Config.gcStepMul);

        if (Config.manualGCMode) {
            Api.lua_gc(L, LUA_GCSTOP, 0);
            Log("[LuaOpt] Auto GC stopped — manual stepping active");
        }

        int memKB = Api.lua_gc(L, LUA_GCCOUNT, 0);
        int memB  = Api.lua_gc(L, LUA_GCCOUNTB, 0);
        State.luaMemoryKB = memKB + (memB / 1024.0);

        State.gcOptimized = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LuaOpt] EXCEPTION in GC optimization");
        Api.lua_gc = nullptr;
        return false;
    }
}

static void RestoreGC(lua_State* L) {
    if (!State.gcOptimized || !Api.lua_gc) return;

    __try {
        Api.lua_gc(L, LUA_GCSETPAUSE,   State.origGCPause);
        Api.lua_gc(L, LUA_GCSETSTEPMUL, State.origGCStepMul);
        Api.lua_gc(L, LUA_GCRESTART, 0);
        Log("[LuaOpt] GC restored: pause=%d, stepmul=%d, auto=ON",
            State.origGCPause, State.origGCStepMul);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LuaOpt] EXCEPTION restoring GC");
    }

    State.gcOptimized = false;
}

// ================================================================
//  4-tier GC Step Size Selection
//
//  Priority: loading > combat > idle > normal
//
//  Why loading is highest priority:
//    During loading screens WoW is not rendering frames,
//    so we can do aggressive GC without affecting frametime.
//    This cleans up garbage BEFORE the player sees the world.
//
//  Why combat is above idle:
//    If somehow both flags are set, combat wins (protect fps).
// ================================================================

static const char* GetGCModeName() {
    if (Config.isLoading) return "loading";
    if (Config.inCombat)  return "combat";
    if (Config.isIdle)    return "idle";
    return "normal";
}

static int GetCurrentStepKB() {
    if (Config.isLoading) return Config.loadingStepKB;
    if (Config.inCombat)  return Config.combatStepKB;
    if (Config.isIdle)    return Config.idleStepKB;
    return Config.normalStepKB;
}

// Per-frame GC step — called from Sleep hook on main thread
static void StepGC(lua_State* L) {
    if (!State.gcOptimized || !Api.lua_gc) return;

    int stepKB = GetCurrentStepKB();

    __try {
        int done = Api.lua_gc(L, LUA_GCSTEP, stepKB);
        State.gcStepsTotal++;

        if (done) {
            State.fullCollects++;
        }

        // Update memory stats every ~64 frames
        State.statsUpdateCounter++;
        if ((State.statsUpdateCounter & 63) == 0) {
            int kb = Api.lua_gc(L, LUA_GCCOUNT, 0);
            int b  = Api.lua_gc(L, LUA_GCCOUNTB, 0);
            State.luaMemoryKB = kb + (b / 1024.0);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LuaOpt] EXCEPTION in GC step — disabling");
        State.gcOptimized = false;
    }
}

// ================================================================
//  Lua Global Helpers
// ================================================================

static void WriteLuaGlobal_Bool(lua_State* L, const char* name, bool value) {
    if (!Api.lua_pushboolean || !Api.lua_setfield) return;
    Api.lua_pushboolean(L, value ? 1 : 0);
    Api.lua_setfield(L, LUA_GLOBALSINDEX, name);
}

static void WriteLuaGlobal_Number(lua_State* L, const char* name, double value) {
    if (!Api.lua_pushnumber || !Api.lua_setfield) return;
    Api.lua_pushnumber(L, value);
    Api.lua_setfield(L, LUA_GLOBALSINDEX, name);
}

static void WriteLuaGlobal_String(lua_State* L, const char* name, const char* value) {
    if (!Api.lua_pushstring || !Api.lua_setfield) return;
    Api.lua_pushstring(L, value);
    Api.lua_setfield(L, LUA_GLOBALSINDEX, name);
}

// Read a boolean global from Lua (returns 0 or 1)
static int ReadLuaGlobal_Bool(lua_State* L, const char* name) {
    if (!Api.lua_getfield || !Api.lua_toboolean || !Api.lua_settop) return 0;
    Api.lua_getfield(L, LUA_GLOBALSINDEX, name);
    int val = Api.lua_toboolean(L, -1);
    Api.lua_settop(L, -2);  // pop
    return val;
}

// ================================================================
//  Read Addon State from Lua Globals
//
//  The addon (LuaBoost) writes these every time state changes:
//    LUABOOST_ADDON_COMBAT  = true/false
//    LUABOOST_ADDON_IDLE    = true/false
//    LUABOOST_ADDON_LOADING = true/false
//
//  We read all three every frame from the Sleep hook.
// ================================================================

static void ReadAddonStateFromLua(lua_State* L) {
    if (!Api.lua_getfield || !Api.lua_toboolean || !Api.lua_settop) return;

    __try {
        bool prevCombat  = Config.inCombat;
        bool prevIdle    = Config.isIdle;
        bool prevLoading = Config.isLoading;

        Config.inCombat  = (ReadLuaGlobal_Bool(L, "LUABOOST_ADDON_COMBAT")  != 0);
        Config.isIdle    = (ReadLuaGlobal_Bool(L, "LUABOOST_ADDON_IDLE")    != 0);
        Config.isLoading = (ReadLuaGlobal_Bool(L, "LUABOOST_ADDON_LOADING") != 0);

        // Log mode transitions (throttled, only when mode actually changes)
        const char* currentMode = GetGCModeName();
        if (currentMode != State.lastModeName) {
            Log("[LuaOpt] GC mode: %s -> %s (step: %d KB/frame)",
                State.lastModeName, currentMode, GetCurrentStepKB());
            State.lastModeName = currentMode;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Non-critical — keep previous state
    }
}

// ================================================================
//  Write DLL Stats to Lua Globals (addon reads these)
// ================================================================

static void UpdateLuaStats(lua_State* L) {
    if (!Api.lua_pushnumber || !Api.lua_setfield) return;

    __try {
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_MEM_KB",      State.luaMemoryKB);
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_GC_STEPS",    (double)State.gcStepsTotal);
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_GC_FULLS",    (double)State.fullCollects);
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_GC_PAUSE",    (double)Config.gcPause);
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_GC_STEPMUL",  (double)Config.gcStepMul);
        WriteLuaGlobal_Number(L, "LUABOOST_DLL_GC_STEP_KB",  (double)GetCurrentStepKB());
        WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_COMBAT",      Config.inCombat);
        WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_IDLE",        Config.isIdle);
        WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_LOADING",     Config.isLoading);
        WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_GC_ACTIVE",   State.gcOptimized);
        WriteLuaGlobal_String(L, "LUABOOST_DLL_GC_MODE",     GetGCModeName());
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================
//  Initial Setup via FrameScript_Execute
// ================================================================

static void SetupLuaInterface(lua_State* L) {
    if (!Api.FrameScript_Execute) {
        if (Api.lua_pushboolean && Api.lua_setfield) {
            WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_LOADED",    true);
            WriteLuaGlobal_String(L, "LUABOOST_DLL_VERSION",   "1.2.1");
            WriteLuaGlobal_Bool(L,   "LUABOOST_DLL_GC_ACTIVE", State.gcOptimized);
            Log("[LuaOpt] Set DLL globals via Lua API (no FrameScript)");
        }
        return;
    }

    __try {
        Api.FrameScript_Execute(
            "LUABOOST_DLL_LOADED = true "
            "LUABOOST_DLL_VERSION = '1.2.1' "

            // Initialize addon state globals if addon hasn't loaded yet
            "if LUABOOST_ADDON_COMBAT  == nil then LUABOOST_ADDON_COMBAT  = false end "
            "if LUABOOST_ADDON_IDLE    == nil then LUABOOST_ADDON_IDLE    = false end "
            "if LUABOOST_ADDON_LOADING == nil then LUABOOST_ADDON_LOADING = false end "

            // Pure Lua helper functions (safe — no C callbacks)
            "function LuaBoostC_IsLoaded() return true end "

            "function LuaBoostC_GetStats() "
            "  return "
            "    LUABOOST_DLL_MEM_KB or 0, "
            "    LUABOOST_DLL_GC_STEPS or 0, "
            "    LUABOOST_DLL_GC_FULLS or 0, "
            "    LUABOOST_DLL_GC_PAUSE or 0, "
            "    LUABOOST_DLL_GC_STEPMUL or 0, "
            "    LUABOOST_DLL_COMBAT or false, "
            "    LUABOOST_DLL_GC_MODE or 'unknown', "
            "    LUABOOST_DLL_IDLE or false, "
            "    LUABOOST_DLL_LOADING or false "
            "end "

            "function LuaBoostC_GCMemory() "
            "  return LUABOOST_DLL_MEM_KB or 0 "
            "end "

            "function LuaBoostC_SetCombat(v) "
            "  LUABOOST_ADDON_COMBAT = v and true or false "
            "end "

            "LUABOOST_DLL_GC_REQUEST = nil "
            "function LuaBoostC_GCStep(kb) "
            "  LUABOOST_DLL_GC_REQUEST = kb or 100 "
            "end "

            "function LuaBoostC_GCCollect() "
            "  LUABOOST_DLL_GC_REQUEST = -1 "
            "end ",
            "LuaOpt", 0
        );

        Log("[LuaOpt] Lua interface created via FrameScript");
        Log("[LuaOpt]   LuaBoostC_IsLoaded()  — returns true");
        Log("[LuaOpt]   LuaBoostC_GetStats()  — reads DLL globals (9 values)");
        Log("[LuaOpt]   LuaBoostC_SetCombat() — writes LUABOOST_ADDON_COMBAT");
        Log("[LuaOpt]   LuaBoostC_GCStep()    — writes LUABOOST_DLL_GC_REQUEST");
        Log("[LuaOpt]   LuaBoostC_GCCollect() — writes LUABOOST_DLL_GC_REQUEST = -1");
        Log("[LuaOpt]   Reads: LUABOOST_ADDON_COMBAT, _IDLE, _LOADING");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[LuaOpt] EXCEPTION in FrameScript_Execute");
        WriteLuaGlobal_Bool(L, "LUABOOST_DLL_LOADED", true);
    }
}

// ================================================================
//  Process GC Requests from Addon
// ================================================================

static void ProcessGCRequests(lua_State* L) {
    if (!Api.lua_getfield || !Api.lua_type || !Api.lua_tonumber ||
        !Api.lua_pushnil || !Api.lua_setfield || !Api.lua_settop || !Api.lua_gc) {
        return;
    }

    __try {
        Api.lua_getfield(L, LUA_GLOBALSINDEX, "LUABOOST_DLL_GC_REQUEST");

        int t = Api.lua_type(L, -1);
        if (t == 3) {  // LUA_TNUMBER
            double val = Api.lua_tonumber(L, -1);
            Api.lua_settop(L, -2);

            // Clear the request
            Api.lua_pushnil(L);
            Api.lua_setfield(L, LUA_GLOBALSINDEX, "LUABOOST_DLL_GC_REQUEST");

            if (val < 0) {
                Api.lua_gc(L, LUA_GCCOLLECT, 0);
                State.fullCollects++;
                Log("[LuaOpt] Addon requested full GC collect");
            } else if (val > 0) {
                Api.lua_gc(L, LUA_GCSTEP, (int)val);
                State.gcStepsTotal++;
            }
        } else {
            Api.lua_settop(L, -2);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ================================================================
//  Main Thread Initialization
// ================================================================

static void DoMainThreadInit() {
    Log("[LuaOpt] ====================================");
    Log("[LuaOpt]  Lua VM Init (main thread)");
    Log("[LuaOpt] ====================================");

    Api.L = ReadLuaState();

    if (!Api.L) {
        Log("[LuaOpt] lua_State* is NULL — Lua VM not ready");
        Log("[LuaOpt] Will retry on next frame");
        InterlockedExchange(&g_luaInitState, 1);
        State.initialized = false;
        return;
    }

    Log("[LuaOpt] lua_State* = 0x%08X", (unsigned)(uintptr_t)Api.L);

    bool gcOk = OptimizeGC(Api.L);

    SetupLuaInterface(Api.L);

    if (Api.lua_pushnumber && Api.lua_setfield) {
        UpdateLuaStats(Api.L);
    }

    State.initialized = true;
    State.lastModeName = GetGCModeName();

    Log("[LuaOpt] ====================================");
    Log("[LuaOpt]  Init Complete");
    Log("[LuaOpt]    GC optimized:     %s", gcOk ? "YES" : "NO");
    Log("[LuaOpt]    Lua interface:    via FrameScript (safe)");
    Log("[LuaOpt]    GC tiers (KB/f):");
    Log("[LuaOpt]      normal  = %d", Config.normalStepKB);
    Log("[LuaOpt]      combat  = %d", Config.combatStepKB);
    Log("[LuaOpt]      idle    = %d", Config.idleStepKB);
    Log("[LuaOpt]      loading = %d", Config.loadingStepKB);
    Log("[LuaOpt] ====================================");
}

// ================================================================
//  Public API
// ================================================================

namespace LuaOpt {

bool PrepareFromWorkerThread() {
    Log("[LuaOpt] ====================================");
    Log("[LuaOpt]  Lua VM Optimizer — Preparing");
    Log("[LuaOpt]  Build 12340 (IDA Pro addresses)");
    Log("[LuaOpt] ====================================");

    if (!ResolveAddresses()) {
        Log("[LuaOpt] Address resolution failed");
        return false;
    }

    HMODULE hWow = GetModuleHandleA(nullptr);
    uintptr_t wowBase = (uintptr_t)hWow;
    Log("[LuaOpt] Wow.exe base: 0x%08X", (unsigned)wowBase);

    if (wowBase != 0x00400000) {
        Log("[LuaOpt] WARNING: Unexpected base! Addresses may be wrong.");
    }

    lua_State* testL = ReadLuaState();
    if (testL) {
        Log("[LuaOpt] lua_State* pre-read: 0x%08X", (unsigned)(uintptr_t)testL);
    } else {
        Log("[LuaOpt] lua_State* = NULL (will retry on main thread)");
    }

    g_addressesValid = true;
    InterlockedExchange(&g_luaInitState, 1);

    Log("[LuaOpt] Ready — waiting for main thread...");
    return true;
}

void OnMainThreadSleep(DWORD mainThreadId) {
    if (GetCurrentThreadId() != mainThreadId) return;

    LONG state = g_luaInitState;

    if (state == 1) {
        if (InterlockedCompareExchange(&g_luaInitState, 2, 1) == 1) {
            DoMainThreadInit();
        }
        return;
    }

    if (state != 2 || !State.initialized || !State.gcOptimized || !Api.L) return;

    // ---- Per-frame work (main thread, between frames) ----

    lua_State* currentL = ReadLuaState();
    if (!currentL) return;

    // Detect UI reload (lua_State pointer changed)
    if (currentL != Api.L) {
        Log("[LuaOpt] lua_State changed (UI reload) — reinitializing");
        Api.L = currentL;
        State.gcOptimized = false;
        State.gcStepsTotal = 0;
        State.fullCollects = 0;
        State.statsUpdateCounter = 0;
        State.lastModeName = "unknown";

        OptimizeGC(Api.L);
        SetupLuaInterface(Api.L);
        return;
    }

    // Read all addon state globals (combat + idle + loading)
    ReadAddonStateFromLua(Api.L);

    // Process any explicit GC requests from addon
    ProcessGCRequests(Api.L);

    // Incremental GC step (4-tier: loading > combat > idle > normal)
    StepGC(Api.L);

    // Update DLL stats in Lua globals (~every 64 frames, ~1 second)
    if ((State.statsUpdateCounter & 63) == 0) {
        UpdateLuaStats(Api.L);
    }
}

void Shutdown() {
    if (!State.initialized) return;

    lua_State* L = ReadLuaState();
    if (L) {
        RestoreGC(L);

        if (Api.lua_pushboolean && Api.lua_setfield) {
            __try {
                WriteLuaGlobal_Bool(L, "LUABOOST_DLL_LOADED", false);
                WriteLuaGlobal_Bool(L, "LUABOOST_DLL_GC_ACTIVE", false);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    Log("[LuaOpt] Shutdown. Total: %d GC steps, %d full collects",
        State.gcStepsTotal, State.fullCollects);

    State.initialized = false;
    InterlockedExchange(&g_luaInitState, 0);
}

void SetCombatMode(bool inCombat) {
    Config.inCombat = inCombat;
}

Stats GetStats() {
    Stats s = {};
    s.initialized        = State.initialized;
    s.gcOptimized        = State.gcOptimized;
    s.allocatorReplaced  = false;
    s.functionsRegistered = true;
    s.luaMemoryKB        = State.luaMemoryKB;
    s.gcStepsTotal       = State.gcStepsTotal;
    s.gcPause            = Config.gcPause;
    s.gcStepMul          = Config.gcStepMul;
    return s;
}

} // namespace LuaOpt