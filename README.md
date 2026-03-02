# 🚀 wow_optimize v1.2 BY SUPREMATIST

**Performance optimization DLL for World of Warcraft 3.3.5a (WotLK)**

Replaces WoW's ancient memory allocator, optimizes I/O, network, timers, threading, frame pacing, and Lua VM garbage collection — all through a single injectable DLL.

> ⚠️ **Disclaimer:** This project is provided as-is for educational purposes. DLL injection may violate the Terms of Service of private servers. **No ban has been reported**, but **use at your own risk.** The authors are not responsible for any consequences including but not limited to account suspensions.

---

## ✨ Features

| # | Feature | What It Does |
|---|---------|--------------|
| 1 | **mimalloc Allocator** | Replaces msvcr80 `malloc`/`free` with Microsoft's modern allocator |
| 2 | **Sleep Hook** | Precise frame pacing via QPC busy-wait (eliminates Sleep jitter) |
| 3 | **TCP\_NODELAY** | Disables Nagle's algorithm on all sockets (lower ping) |
| 4 | **GetTickCount Hook** | QPC-based microsecond precision (better internal timers) |
| 5 | **CriticalSection Spin** | Adds spin count to all locks (fewer context switches) |
| 6 | **ReadFile Cache** | 64KB read-ahead cache for MPQ files (faster loading) |
| 7 | **CreateFile Hints** | Sequential scan flags for MPQ (OS prefetch optimization) |
| 8 | **CloseHandle Hook** | Cache invalidation on file close (prevents stale data) |
| 9 | **Timer Resolution** | 0.5ms system timer via NtSetTimerResolution |
| 10 | **Thread Affinity** | Pins main thread to optimal core (stable L1/L2 cache) |
| 11 | **Working Set** | Locks 256MB–2GB in RAM (prevents page-outs) |
| 12 | **Process Priority** | Above Normal + disabled priority boost |
| 13 | **FPS Cap Removal** | Raises hardcoded 200 FPS limit to 999 |
| 14 | **Lua VM Optimizer** | Per-frame GC stepping from C, combat-aware, auto UI reload detection |

---

## 🤔 What Changes In Practice

This is **not** a magic FPS doubler. Think of it like replacing an HDD with an SSD — same benchmarks, but everything *feels* smoother.

### You WILL notice

- ✅ Fewer random micro-stutters (especially Lua GC stalls)
- ✅ More stable minimum FPS (less variance between frames)
- ✅ Smoother frame pacing (no more Sleep jitter)
- ✅ Less lag degradation over long sessions (2+ hours)
- ✅ Lower network latency (spells feel more responsive)
- ✅ Faster zone loading

### You WON'T notice

- ✗ Average FPS won't jump dramatically
- ✗ No visual changes
- ✗ No in-game notifications

### Where it matters most

- 🏰 Dalaran / Stormwind with many players
- ⚔️ 25-man raids (ICC, RS) with heavy addon usage
- ⏱️ Long play sessions without restarting the client
- 🌐 High-latency connections (TCP\_NODELAY helps most here)

---

## 🔧 Recommended Combo

For maximum optimization, use this DLL together with the **[!LuaBoost](https://github.com/suprepupre/LuaBoost)** addon:

| Layer | Tool | What It Does |
|-------|------|--------------|
| **C / Engine** | wow\_optimize.dll | Faster memory, I/O, network, timers, threads, Lua GC from C |
| **Lua / Addons** | !LuaBoost addon | Faster math/table, incremental GC, table pool, throttle API |

When both are installed, the DLL handles Lua GC stepping from C (zero Lua overhead) and communicates with the addon via shared globals. The addon provides the GUI, combat awareness, idle detection, and runtime function optimizations.

> ⚠️ **Do NOT use [SmartGC](https://github.com/suprepupre/SmartGC) together with !LuaBoost** — SmartGC has been merged into LuaBoost. Using both will cause conflicts.

---

## 📦 Building

### Requirements

- **Windows 10/11**
- **Visual Studio 2022** (or 2019) with **"Desktop development with C++"** workload
- **CMake** (included with Visual Studio)
- **Internet connection** (first build downloads dependencies automatically)

### Build Steps

```bash
git clone https://github.com/suprepupre/wow-optimize.git
cd wow-optimize
build.bat
```

Output: `build\Release\wow_optimize.dll` + `build\Release\version.dll`

> ⚠️ **Must be compiled as 32-bit (Win32).** WoW 3.3.5a is a 32-bit application.

### Manual Build

```bash
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A Win32 ..
cmake --build . --config Release
```

---

## 📥 Quick Install (No Building Required)

Download pre-built binaries from [**Releases**](../../releases/latest).

---

## 🎮 Usage

### Option A — Auto-Load (recommended)

1. Copy `version.dll` and `wow_optimize.dll` to your WoW folder
2. Launch WoW normally
3. Done — loads automatically every time

### Option B — Manual Injection

1. Copy `wow_optimize.dll`, `Dll_Injector.exe`, `inject.bat` to WoW folder
2. Launch WoW, wait for login screen
3. Double-click `inject.bat`

### Verify

Check `wow_optimize.log` — all lines should show `[ OK ]`.

```
[22:59:07.520] ========================================
[22:59:07.520]   wow_optimize.dll v1.2 BY SUPREMATIST
[22:59:07.520]   PID: 31380
[22:59:07.520] ========================================
[22:59:07.520] MinHook initialized
[22:59:07.528] mimalloc configured (large pages, pre-warmed 64MB)
[22:59:07.606] >>> ALLOCATOR: mimalloc ACTIVE <<<
[22:59:07.685] Sleep hook: ACTIVE (precise busy-wait + Lua GC stepping)
[22:59:07.685] GetTickCount hook: ACTIVE
[22:59:07.685] CriticalSection hook: ACTIVE
[22:59:07.763] Network hook: ACTIVE
[22:59:07.763] CreateFile hooks: ACTIVE
[22:59:07.763] ReadFile hook: ACTIVE
[22:59:07.764] Timer resolution: 0.500 ms
[22:59:07.840] Main thread: ideal core 1, priority HIGHEST
[22:59:07.840] Process: Above Normal priority
[22:59:07.840] Working set: min 256 MB, max 2048 MB
[22:59:07.840] FPS cap: changed from 200 to 999
[22:59:07.840]
[22:59:07.840] --- Lua VM Optimizer ---
[22:59:07.841] [LuaOpt] Resolving addresses for build 12340...
[22:59:07.841] [LuaOpt]   lua_gc                 0x0084ED50  OK
[22:59:07.841] [LuaOpt]   lua_State* ptr         0x00D3F78C  OK (data)
[22:59:07.841] [LuaOpt] Resolved: 14 OK, 0 FAILED
[22:59:07.841] [LuaOpt] Ready — waiting for main thread
[22:59:07.842]
[22:59:07.842] ========================================
[22:59:07.842]   Initialization complete
[22:59:07.842] ========================================
[22:59:07.842]
[22:59:07.842]   [ OK ] mimalloc allocator
[22:59:07.842]   [ OK ] Sleep hook (frame pacing)
[22:59:07.842]   [ OK ] GetTickCount (precision)
[22:59:07.842]   [ OK ] CriticalSection (spin lock)
[22:59:07.842]   [ OK ] TCP_NODELAY (network)
[22:59:07.842]   [ OK ] CreateFile (sequential I/O)
[22:59:07.842]   [ OK ] ReadFile (MPQ read-ahead)
[22:59:07.842]   [ OK ] CloseHandle (cache cleanup)
[22:59:07.842]   [ OK ] Timer resolution (0.5ms)
[22:59:07.842]   [ OK ] Thread affinity + priority
[22:59:07.842]   [ OK ] Working set (256MB-2GB)
[22:59:07.842]   [ OK ] Process priority (Above Normal)
[22:59:07.842]   [ OK ] FPS cap removal (200 -> 999)
[22:59:07.842]   [WAIT] Lua VM GC optimizer
[22:59:07.860]
[22:59:07.860] [LuaOpt] Lua VM Init (main thread)
[22:59:07.860] [LuaOpt] lua_State* = 0x1A2B3C4D
[22:59:07.860] [LuaOpt] lua_gc verified: Lua memory = 24576 KB
[22:59:07.860] [LuaOpt] GC tuned: pause 200->110, stepmul 200->300
[22:59:07.860] [LuaOpt] Auto GC stopped
[22:59:07.860] [LuaOpt] Lua interface created via FrameScript
[22:59:07.860] [LuaOpt] Init complete — GC:YES, step:64/16 KB/frame
```

### Uninstall

Delete `version.dll` (and `wow_optimize.dll`) from WoW folder.

---

## 🧠 Lua VM Optimizer (v1.2)

New in v1.2: The DLL now directly optimizes WoW's Lua 5.1 garbage collector from C code.

### How It Works

1. **Address Discovery** — Finds Lua C API functions in Wow.exe using IDA Pro addresses (build 12340)
2. **GC Parameter Tuning** — Sets `pause=110` (collect sooner) and `stepmul=300` (collect faster)
3. **Manual Stepping** — Stops auto-GC, performs controlled incremental steps every frame from the Sleep hook
4. **Combat Awareness** — Reads combat state from the LuaBoost addon, reduces GC during fights (16 KB vs 64 KB per frame)
5. **Addon Communication** — Writes stats to Lua globals, creates pure Lua wrapper functions via `FrameScript_Execute`
6. **UI Reload Handling** — Detects `lua_State*` changes and reinitializes automatically

### DLL ↔ Addon Communication

```
DLL writes → Lua globals (every ~64 frames):
  LUABOOST_DLL_LOADED      = true
  LUABOOST_DLL_MEM_KB      = 24576.5
  LUABOOST_DLL_GC_STEPS    = 18432
  LUABOOST_DLL_GC_ACTIVE   = true

Addon writes → Lua globals (on events):
  LUABOOST_ADDON_COMBAT    = true/false
  LUABOOST_DLL_GC_REQUEST  = 256 (step) or -1 (full collect)

DLL reads addon globals per-frame from the Sleep hook (main thread).
```

> **Why not `lua_pushcclosure`?** WoW validates C function pointers passed to Lua. If the pointer is outside Wow.exe's code section, WoW crashes with `Fatal condition: Invalid function pointer`. The DLL creates pure Lua wrapper functions via `FrameScript_Execute` instead — these are safe.

---

## 🧠 Technical Details

### Safe Allocator Transition

```
Before injection:  malloc() → old CRT heap
After injection:   malloc() → mimalloc heap
                   free()   → checks which heap owns the pointer
                              ├── mimalloc → mi_free()
                              └── old CRT  → original free()
```

### CRT Auto-Detection

| DLL | Compiler |
|-----|----------|
| `msvcr80.dll` | Visual C++ 2005 (original WoW 3.3.5a) |
| `msvcr90.dll` | Visual C++ 2008 |
| `msvcr100-120.dll` | Visual C++ 2010-2013 |
| `ucrtbase.dll` | Visual C++ 2015+ |
| `msvcrt.dll` | System CRT |

### Dependencies

All downloaded automatically by CMake on first build.

| Library | Version | Purpose | License |
|---------|---------|---------|---------|
| [mimalloc](https://github.com/microsoft/mimalloc) | 3.2.8 | Memory allocator | MIT |
| [MinHook](https://github.com/TsudaKageyu/minhook) | latest | Function hooking | BSD 2-Clause |

---

## ⚠️ Important Notes

### Anti-Cheat (Warden)

**No bans have been reported.** However, DLL injection is inherently detectable.

What this DLL does **NOT** do:

- ❌ Does not modify `Wow.exe` on disk
- ❌ Does not provide any gameplay advantage
- ❌ Does not read/write game-specific memory (packets, player data)
- ❌ Does not automate gameplay

What this DLL **does**:

- ✅ Hooks system-level functions (`malloc`, `free`, `Sleep`, `connect`, `ReadFile`)
- ✅ Calls Lua VM GC API for performance tuning (read-only stats + GC stepping)

### System Requirements

- 32-bit compilation only (WoW 3.3.5a is 32-bit)
- Compatible with DXVK and LAA patch
- Inject after login screen appears

---

## 🐛 Troubleshooting

### Proxy `version.dll` not loading

**Symptom:** You placed `version.dll` and `wow_optimize.dll` next to `Wow.exe`, but no `wow_optimize.log` is created on launch — the DLL is not being picked up.

**Fix:** UNCHECK "Disable fullscreen optimizations" for `Wow.exe`:

1. Right-click `Wow.exe` → **Properties**
2. Go to the **Compatibility** tab
3. UNCHECK **"Disable fullscreen optimizations"**
4. Click **Apply** → **OK**
5. Relaunch WoW

> Windows fullscreen optimizations can interfere with how the application loads DLLs from its working directory. Disabling this feature forces Windows to use the classic DLL search order, allowing the proxy `version.dll` to be loaded correctly.

![Disable fullscreen optimizations](screenshots/wow.exe%20properties.png)

---

| Problem | Solution |
|---------|----------|
| Proxy DLL doesn't load (no log file) | See above — UNCHECK "disable fullscreen optimizations" on `Wow.exe` |
| WoW crashes after injection | Wait for login screen + 10 seconds before injecting |
| `FATAL: MinHook initialization failed` | Another hook DLL conflicting |
| `ERROR: No CRT DLL found` | Non-standard WoW build |
| Lua optimizer shows `SKIP` | Lua addresses not found (different build?) |
| `Invalid function pointer` crash | Old DLL version — update to v1.2+ |
| `Large pages: no permission` | Normal — requires admin policy change, optional |
| No noticeable difference | Expected on high-end PCs |

---

## 📁 Project Structure

```
wow-optimize/
├── src/
│   ├── dllmain.cpp          # Main DLL — all system hooks
│   ├── lua_optimize.cpp     # Lua VM GC optimizer module
│   ├── lua_optimize.h       # Lua optimizer interface
│   ├── version_proxy.cpp    # Auto-loader (version.dll proxy)
│   └── version_exports.def  # Export definitions for version.dll
├── CMakeLists.txt           # Build config + dependency management
├── build.bat                # One-click build script
├── README.md
├── LICENSE
└── .gitignore
```

---

## 📜 License

MIT License — use, modify, and distribute freely. See [LICENSE](LICENSE) for full text.