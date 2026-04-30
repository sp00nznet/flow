# flOw — Static Recompilation

> The first native PC port of thatgamecompany's *flOw*, rebuilt from the PS3 binary through static recompilation.

**flOw** (2007) was the debut PlayStation Network title from thatgamecompany — the studio that went on to create *Flower*, *Journey*, and *Sky*. Originally a browser Flash game by Jenova Chen, the PS3 version was rebuilt on Sony's PhyreEngine with 1080p rendering, SIXAXIS tilt controls, and a hypnotic ambient soundtrack. It never received a PC release.

Until now.

This project takes the PS3 `EBOOT.elf` binary, disassembles all PowerPC functions, lifts them to C, and links against [ps3recomp](https://github.com/sp00nznet/ps3recomp) — a set of HLE runtime libraries that replace the PS3 operating system with native host implementations. The result is a standalone Windows executable that runs flOw natively, no emulator required.

## Current Status

| Metric | Value |
|---|---|
| Title | flOw |
| Title ID | NPUA80001 |
| Engine | PhyreEngine (Sony) |
| Functions recompiled | 102,056 (OPD + heuristic + branch target splitting + targeted re-lifts) |
| Functions lifted to C | 102,056 (100%) |
| Trampoline sites | 22,000 converted fallthroughs, 143,000 drain sites |
| Imported libraries | 12 |
| Imported functions | 140/140 resolved (100%) |
| Binary size | ~10 MB (ELF) → ~45 MB (exe) |
| HLE bridges | 7/12 real (cellSysutil, cellGcmSys, cellAudio, cellPad, cellFs, cellSysmodule, sysPrxForUser) |
| Remaining TODOs | ~10,000 (mostly VMX comparison + remaining unrecognized op4) |
| Indirect calls | 20,030 bctrl sites → hash table + 3-level OPD dispatch for C++ virtual methods |
| ps3recomp version | v0.4.0+ |
| Target | Windows x86-64 (Linux planned) |

### Phase Progress

| Phase | Status | Notes |
|---|---|---|
| PKG extraction | **Complete** | 553 files extracted from PSN package |
| SELF decryption | **Complete** | EBOOT.BIN → EBOOT.elf via RPCS3 + RAP |
| Binary analysis | **Complete** | 91,758 functions via OPD + heuristic + branch target splitting |
| NID resolution | **Complete** | 140/140 import NIDs mapped to function names |
| ELF structural analysis | **Complete** | Segments, sections, OPD, TOC, memory map |
| PPU lifting | **Complete** | 91,758 functions lifted to C++ (190 MB source) |
| Runtime linking | **Complete** | Builds and links against ps3recomp runtime |
| HLE module registration | **Complete** | 12 modules, 7 with real HLE bridges |
| CRT startup | **Complete** | TLS → mutexes → malloc → static constructors |
| CRT abort redirect | **Complete** | longjmp workaround for constructor stack overflows |
| Game main() | **Complete** | Loads 7 modules, registers sysutil callback, initializes SPU |
| GCM / RSX init | **Complete** | Guest command buffer, display buffers, tile/zcull, MapMainMemory |
| Engine init | **Complete** | PhyreEngine created, 12 subsystems, vtables resolved |
| Input init | **Complete** | cellPadInit, cellKbInit (×4), cellMouseInit |
| Engine game loop | **Running** | Continuous frame loop, ~88 FPS, 12 subsystems ticking each frame |
| Natural cellGcmInit | **Working** | Game's own `_cellGcmInitBody` fires; cmdbuf 0x00C005E0–0x00CF05E0 |
| Natural VBlank handler | **Working** | Game's `func_000CBE24` dispatched per frame via `g_ps3_guest_caller` |
| GCM rendering | **Working** | D3D12 GPU: game's own surface clear (placeholder scene-builder gated off once natural ctx is live) |
| Buffer flips | **Working** | cellGcmSetFlipCommand alternating buffers 0/1, batched DRAW_ARRAYS |
| Graphics backend | **D3D12** | Device FL11.0, vertex-colored PSO, 112KB VB, DrawInstanced, VSync |
| Audio backend | **Wired** | cellAudio → WASAPI via ps3recomp |
| Input backend | **Wired** | cellPad → XInput via ps3recomp |
| SPURS pre-seed | **Working** | `cellSpursInitialize` called at engine_run entry on synthesized 2 KB struct |
| SPU PPU fallback | **Available** | ps3recomp infrastructure complete; flOw doesn't yet trigger SPU thread create |
| FIFO sync | **Partial** | All-register flag clearing solves 2/3 spin levels in PhyreEngine init |
| Title-state machine | **Stuck** | Engine alive, no `cellPadGetData` / `cellHddGameCheck` / `cellFsOpen` / `sys_ppu_thread_create` — natural worker thread for the per-frame loop never spawns |
| Full gameplay | In Progress | Title state needs upstream init we haven't located (gated by `r30` = game-state struct we can't synthesize) |

### What Works Now

- **D3D12 GPU rendering** — Level 3 scene: ocean gradient, 9-segment snake creature, 16 food objects, 40 particles (486 vertices via batched DRAW_ARRAYS)
- **Level data parsing** — reads flOw XML level definitions for authentic background colors, creature parameters, particle counts
- **Continuous frame loop** — 800+ frames, ~17fps with D3D12 VSync, snapshot restore every 10 frames
- **12 subsystems registered and ticking** — engine internal array populated, types 10-19 assigned
- **Inline switch table cases** — 10 subsystem tick interpolation cases implemented directly in C
- **Full GCM command pipeline** — NV40 commands → gcm_flush → rsx_process_command_buffer → D3D12 backend
- **Batched draw calls** — DRAW_ARRAYS split into 255-vertex batches for RSX method format
- **RSX command processor** — parses NV40 methods (surface, viewport, clear, attribs, draw, begin/end)
- **FIFO flag clearing** — clears PhyreEngine pending flags on r27/r28/r30/r26/r29/r31 (+0x18)
- **FIFO watchdog thread** — monitors ctrl->put, scans for SET_REFERENCE, patches NULL OPDs
- **D3D12 backend** — Device FL11.0, vertex-colored PSO, 112KB VB, DrawInstanced, VSync present
- **102,056 functions** recompiled to native C++ (base lift + 2 targeted re-lifts)
- **PhyreEngine initialized** — 12 subsystems, engine vtable 0x1006E508, game instance at BSS 0x10163764
- **GCM / RSX initialized** — guest command buffer (960KB), display buffers, tile, zcull, IO mapping
- **Input system initialized** — cellPadInit, cellKbInit (4 code types), cellMouseInit
- **3-level OPD dispatch** for C++ virtual method calls
- **ELF data snapshot system** — protects 1.3MB of zeroed GOT entries, OPDs, and vtables
- **gCellGcmCurrentContext restoration** — survives snapshot restores via GOT + BSS chain fix
- **Control register emulation** — host-endian put/get/ref with per-put-change ref increment
- **AddressToOffset fallback** — handles unmapped heap addresses for GCM operations
- **CRT abort redirect via longjmp** — intercepts CRT abort, recovers into main()
- **166 static constructors** executing via indirect call dispatch
- **VMX/AltiVec** — vector loads, stores, float arithmetic, permute, select, compare all working
- **7 HLE modules with real bridges** — PPC64 ABI parameter extraction, BE struct output, host OS backends
- **HLE malloc** — bump allocator bypassing CRT's Dinkumware heap
- **D3D12 window** — opens on startup with Win32 message pump, RSX null/D3D12 backend ready
- **Real WASAPI audio**, **XInput gamepad**, **filesystem I/O**, **lightweight mutexes**
- **LV2 syscall dispatch** with sys_tty_write/read, event ports, timers

### Known Issues

- **Title-state wall** — engine alive but state machine doesn't progress. Disasm-verified: natural `func_000C858C` is one-shot shutdown, not a loop. The natural per-frame outer loop must come from a worker thread spawned by init code we don't reach. `sys_ppu_thread_create` is never called naturally; `cellHddGameCheck` / `cellFsOpen` / `cellPadGetData` zero calls. Our `for(;;)` substitutes for the missing worker — works for ticking but doesn't trigger the state machine.
- **`func_00810BB8` is `__cxa_pure_virtual`** — engine vtable[2] is the abstract-method abort stub. Confirmed via disasm. Calling it always aborts. Kept disabled.
- **`func_00138B7C` → `cellHddGameCheck` path** — gated by `sys_memory_container_create` (syscall 0x155); failure jumps to `func_001392F0` which calls cellHddGameCheck with a callback OPD at `r30 + -0x7DDC`. Reaching it from our injection point needs `r30` = a game-state struct we haven't synthesized.
- **Render method callbacks** — Engine vtable[2] disabled; our scene-builder gated off once natural GCM ctx is live.
- **Empty scene graph** — Render context exists but render_ctx+0x3C4 (scene data) has an empty cluster.
- **Switch table targets** — func_000CA3B4's switch dispatch (types 10-24) targets not in function table. Worked around with inline `subsystem_tick_case()`. Lifter needs switch table target extraction.
- **ELF data zeroing** — Workaround: snapshot restore at 6+ points + gCellGcmCurrentContext chain restore + BSS 0x10112000-0x10170000 zeroing.
- **Control register endianness** — RSX MMIO accessed via lwbrx/stwbrx → host-endian uint32_t* access.
- **vm_read32_fast bypass** — Snapshot restores via direct memcpy to vm_base.

### Build Pipeline (Fully Automated)

```bash
# 1. Run the lifter (~2-3 hours for 92K functions)
python tools/recompile.py --skip-find --functions-file combined_functions_split.json \
    --ps3recomp-dir D:/recomp/ps3

# 2. Post-process (rename, patch header, fallthrough, bctrl, malloc)
python tools/post_lift.py --recomp-dir src/recomp

# 3. Convert fallthroughs to trampolines (22K conversions, 143K drain sites)
python tools/convert_trampolines.py --recomp-dir src/recomp

# 4. Generate missing stubs
python tools/gen_stubs.py --recomp-dir src/recomp

# 5. Build
cmake -B build -DPS3RECOMP_DIR=D:/recomp/ps3
cmake --build build --config Release
```

### Technical Discoveries

These findings apply to ALL ps3recomp game ports:

1. **TOC save in import stubs** — The PPC64 ABI requires saving r2 (TOC) to sp+40 before inter-module calls. The lifter doesn't emit this instruction. Import stubs must save TOC in their `nid_dispatch()` function or all subsequent TOC-relative loads crash.

2. **Split-function fallthrough** — The lifter splits PPC functions at boundary points, creating multiple C functions. When a function ends without `blr`/`b`, the lifter must emit a call to the next function. Without this, function prologues return before executing their body.

3. **dcbz must zero memory** — `dcbz` (data cache block zero) is NOT safe to no-op. Games use dcbz loops for bulk memory initialization. No-oping dcbz creates infinite loops (CTR computed from huge byte counts) and leaves memory regions uninitialized, causing later crashes.

4. **Guest malloc bypass** — The PS3 CRT's Dinkumware malloc requires OS-level heap initialization that recomp doesn't replicate. Replace with a bump allocator in committed guest VM memory.

5. **Initial LR** — Set ctx.lr to the `sys_process_exit` import stub address before entering the game. The PS3 OS sets LR to the exit handler; leaving it at 0 propagates NULL through the entire CRT stack.

6. **CRT abort redirect via longjmp** — PS3 CRT static constructors can overflow the guest stack due to split-function backward branch recursion. Rather than fixing every constructor, install a SEH handler that catches the access violation and longjmp back to main() with a clean stack. The game still initializes correctly because the constructors that crash are non-essential.

7. **Trampoline drain sites** — Split-function fallthroughs need a trampoline mechanism: after every `bl` call, insert a DRAIN_TRAMPOLINE macro that checks if the callee set a "fall-through target" flag and jumps to the next split fragment. Without this, returning from a split function resumes at the wrong point.

8. **Backward branch recursion across split boundaries** — When the lifter splits a function and encounters a backward branch to an address in a different split fragment, it emits a function call instead of a branch. This creates mutual recursion between fragments, blowing the host stack. Root cause of most constructor crashes.

9. **Manual dispatch stubs for mid-function entry points** — Some indirect calls target addresses in the middle of lifted functions (not function entry points). These require hand-written dispatch stubs that set up context and jump to the correct offset within the target function.

## Import Map

flOw imports 140 functions from 12 PS3 system libraries:

| Library | Functions | HLE Status | Key APIs |
|---|---|---|---|
| **sysPrxForUser** | 15 | **Real** | TLS init, lwmutex create/lock/unlock/destroy, thread create, process exit, time |
| **cellSysutil** | 15 | **Real** | RegisterCallback, GetSystemParamInt/String, VideoOutGetState/GetResolution/Configure |
| **cellGcmSys** | 18 | **Real** | Init, GetConfiguration, GetControlRegister, SetDisplayBuffer, MapMainMemory, SetTile, SetFlip |
| **cellSysmodule** | 4 | **Real** | LoadModule/UnloadModule with module ID name logging |
| **cellAudio** | 7 | **Real** | Init, PortOpen/Close/Start/Stop, GetPortConfig (WASAPI backend) |
| **sys_io** | 17 | **Real** | cellPadInit/End/GetData/GetInfo (XInput), keyboard/mouse stubs |
| **sys_fs** | 11 | **Real** | cellFsOpen/Close/Read/Write/Lseek/Fstat/Opendir (host filesystem) |
| **sys_net** | 21 | Stubbed | BSD sockets — not needed for offline play |
| **cellSpurs** | 14 | Stubbed | SPU workload management |
| **sys_fs** | 11 | Stubbed | File I/O — open, read, write, close, stat |
| **sceNp** | 10 | Stubbed | PlayStation Network services |
| **cellAudio** | 7 | Stubbed | Audio port management |
| **cellNetCtl** | 5 | Stubbed | Network control |
| **cellSysmodule** | 4 | **Real** | Module load/unload with logging |
| **cellSync** | 3 | Stubbed | Barrier synchronization |

## How It Works

```
+-----------------------------------------------------------+
|                    flOw EBOOT.elf                          |
|              (PowerPC 64-bit, big-endian)                  |
+-----------------------------+-----------------------------+
                              |
                  +-----------v-----------+
                  | Analyze (18K+37K OPD) |  91,758 functions
                  +-----------+-----------+
                              |
                    +---------v---------+
                    | Lift (ppu_lifter) |  ~190 MB of C++ code
                    +---------+---------+
                              |
               +--------------v--------------+
               |    Link & Compile           |
               |                             |
               |  ppu_recomp.cpp (~190 MB)   |
               |  + import_stubs.cpp         |
               |  + hle_modules.cpp          |
               |  + vm_bridge.cpp            |---> flow.exe (37 MB)
               |  + ps3recomp runtime        |
               |  + elf_loader.cpp           |
               +--------------+--------------+
```

### Architecture

- **vm_bridge.cpp** — Bridges lifter's uint64_t memory API to ps3recomp's uint32_t big-endian VM
- **hle_modules.cpp** — Registers 12 PS3 modules with NID→handler mappings for HLE dispatch + longjmp abort redirect
- **indirect_dispatch.cpp** — bctrl hash table + trampoline system for split-function fallthroughs
- **import_stubs.cpp** — 140 import stub functions that resolve NIDs at runtime
- **malloc_override.cpp** — Bump allocator (0x00A00000 - 0x10000000) bypassing Dinkumware CRT heap
- **elf_loader.cpp** — Loads PT_LOAD segments from EBOOT.elf into virtual memory
- **ppu_recomp.cpp** — 91,758 lifted C++ functions (auto-generated, ~190 MB source)
- **func_table.cpp** — RecompiledFunc array mapping guest addresses to host functions

### Key Technical Details

- **TOC base**: `0x008969A8` (from OPD analysis)
- **Entry point**: OPD `0x00846AE0` → CRT stub `0x00010230` → `0x00010254`
- **Memory map**: Text (8 MB @ 0x10000), Data (0.5 MB @ 0x820000), Rodata (0.2 MB @ 0x10000000), Data+BSS (3.3 MB @ 0x10040000)
- **ppu_context**: Uses runtime's full struct (GPR, FPR, VR, CR, LR, CTR, XER, FPSCR, CIA)

## Building

### Prerequisites

- Python 3.8+
- CMake 3.20+
- MSVC 2022 (or Clang/GCC)
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (at `../ps3/` or set `PS3RECOMP_DIR`)

### Steps

```bash
# Clone
git clone https://github.com/sp00nznet/flow.git
cd flow

# Place your decrypted EBOOT.elf in game/
# (You need a legitimate copy of flOw from PSN)

# Run the recompilation pipeline (takes ~2-3 hours for 92K functions)
python tools/recompile.py --functions-file combined_functions.json \
    --skip-find --ps3recomp-dir ../ps3

# Patch generated header (runtime ppu_context + extern C)
# (see src/recomp/ppu_recomp.h for required modifications)

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_DIR=../ps3
cmake --build build --config Release

# Run
./build/Release/flow.exe game/
```

> **Note**: You must supply your own copy of flOw (NPUA80001). This repo contains only the recompilation toolchain and runtime code, not any copyrighted game data.

## Project Structure

```
flow/
├── README.md
├── CMakeLists.txt              # Build config (links ps3recomp runtime)
├── config.toml                 # Recompiler pipeline configuration
├── analysis.json               # 18,159 function boundaries (heuristic)
├── combined_functions.json     # 51,658 functions (analysis + OPD)
├── opd_functions.json          # 36,827 OPD procedure descriptors
├── elf_analysis.json           # Full ELF structural analysis
├── game/                       # Place EBOOT.elf + Data/ here (not in repo)
├── src/
│   ├── main.cpp                # Entry point, VM/syscall/HLE initialization
│   ├── stubs.cpp               # Placeholder function table (no-recomp mode)
│   ├── config.h                # Build-time constants
│   ├── elf_loader.cpp/h        # ELF segment loader
│   ├── vm_bridge.cpp           # Memory API bridge (uint64→uint32, byte swap)
│   ├── hle_modules.cpp         # HLE module registration (12 modules, 140 NIDs)
│   ├── indirect_dispatch.cpp   # bctrl hash table + trampoline drain system
│   ├── malloc_override.cpp     # Bump allocator bypassing CRT heap
│   └── recomp/                 # Generated recompiled code (not in repo)
│       ├── ppu_recomp.cpp      # 91,758 lifted functions (~190 MB)
│       ├── ppu_recomp.h        # Context struct + declarations
│       ├── func_table.cpp      # Guest→host function mapping
│       ├── import_stubs.cpp    # 140 NID-dispatching import stubs
│       └── missing_stubs.cpp   # Empty stubs for unlifted call targets
├── tools/
│   ├── recompile.py            # Master recompilation pipeline
│   ├── post_lift.py            # Post-process: rename, patch header, fallthrough, bctrl
│   ├── convert_trampolines.py  # Fallthrough → trampoline conversion (22K sites)
│   └── gen_stubs.py            # Generate missing function stubs
├── extracted/                  # Game metadata and analysis outputs
│   └── USRDIR/imports.json     # Resolved import table
├── parse_imports_final.py      # ELF import table parser (140/140 NIDs)
├── analyze_elf.py              # Comprehensive ELF analyzer
└── extract_pkg.py              # PKG extraction tool
```

## Related Projects

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — The PS3 HLE runtime library this project builds against
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — The project that proved static recompilation works at scale
- **[RPCS3](https://github.com/RPCS3/rpcs3)** — PS3 emulator whose HLE research makes this possible

## Legal

This project does not contain any proprietary Sony code, game binaries, encryption keys, or copyrighted game assets. It is a clean-room reimplementation of PS3 system libraries paired with automated binary translation tools. Users must supply their own legally obtained copy of flOw.

## Changelog

### v0.5.0 — Natural Init + SPU Foundation (2026-04-29)
- **Natural `_cellGcmInitBody` fires** — game's own GCM init runs to completion: cmdbuf at 0x00C005E0–0x00CF05E0 (960 KB), VBlank handler installed, FlipMode set, double-buffered, IO mapping done.
- **Game-registered VBlank handler runs per frame** via cross-repo `g_ps3_guest_caller` — `cellGcmTickVBlank()` from our hand-edit dispatches the game's `func_000CBE24` every frame. Handler increments TOC-0x54D0 and sets the game-state flag at TOC-0x54CC every Nth tick.
- **Engine vtable[3] dispatch** (`func_000CAA08`, the subsystem walker) ticks per frame without exception. 12 subsystems registered into `engine+4..0x30`. Per-subsystem vtable[3] also dispatched per frame.
- **Scene-builder handover** — once the game's natural GCM context is detected at `0x101ED198`, our placeholder scene-builder is gated off (`s_natural_gcm_active`). The game's own surface clear (dark blue) is what's now visible.
- **Direct flip stays on** — runtime processes whatever the game emitted into the natural cmdbuf each frame; D3D12 backend presents.
- **~88 FPS sustained**, no aborts, no asserts, indefinite stability.
- **Disasm-verified the natural lifecycle**: `func_000C858C` is genuinely one-shot shutdown (subsystem dtor walk + `cellPadEnd/KbEnd/MouseEnd` + tail-call destructor). The game's per-frame outer loop must come from a worker thread spawned by init code we haven't reached. Our hand-edit's `for(;;)` in `func_000C858C` substitutes for that missing worker.
- **Pre-init SPURS seed** — calls `cellSpursInitialize` at engine_run entry on a synthesized 2 KB CellSpurs object so any subsystem that gates on the SPURS magic is satisfied.
- **vtable[2] is `__cxa_pure_virtual`** — confirmed: `func_00810BB8` is the abstract-method abort stub. Calling it is always wrong; kept disabled with documented reasoning.

#### ps3recomp SPU Stack (sister repo, 11 commits this cycle)
The flOw push surfaced gaps in ps3recomp's SPU/SPURS support. Added a complete PPU-side execution path for any SPU-dependent game:
- `nid_database.py`: 10 → 100+ SPURS NIDs (Attribute*, Taskset*, Task*, Workload*, EventFlag*, JobChain*, LFQueue*).
- `ps3emu/spu_fallback.h` registry — `spu_register_ppu_fallback(entry_point, handler, user)`. Per-game shims register PPU-side equivalents of SPU jobs.
- `sys_spu_image_open` parses SPU ELF — validates 0x7F"ELF" magic, extracts `e_entry` (BE u32 at offset 24), writes to `image+4`. Fallback registry can now match by real entry point.
- SPU thread `args_ea` (gpr[8] in `sys_spu_thread_initialize`) plumbed through to fallback handlers; `sys_spu_thread_set_argument` updates it.
- **Async fallback execution** — `sys_spu_thread_group_start` spawns one host thread per SPU thread that has a registered fallback (Win32 `CreateThread` / POSIX `pthread_create`). `sys_spu_thread_group_join` blocks on each thread's finish event and collects worst exit status.
- `sys_spu_thread_get_exit_status` returns proper `CELL_ESRCH` / `CELL_ESTAT` / `CELL_OK` semantics (matches Sony docs).
- **Virtual 256 KB local store per SPU thread** — `sys_spu_thread_write_ls` / `_read_ls` work end-to-end with 1/2/4/8-byte BE accesses + bounds checks. `spu_thread_get_local_store(tid)` exposes it to fallback handlers, closing the producer/consumer loop.
- **Event-queue completion notification** — `sys_spu_thread_group_connect_event[_all_threads]` records the queue ID; `group_join` pushes a completion event `{group_id, exit_status, cause, 0}` so PPU code blocked in `sys_event_queue_receive` wakes up.
- New `sys_event_queue_push_by_id()` public helper for non-syscall callers.
- `docs/SPU_FALLBACK.md` with usage example + LS access pattern.
- vt2 watchdog (jmp_buf + `g_vt2_in_progress` flag in `bridge_sys_lwmutex_lock`) for safe spin investigation.

### v0.4.0 — Game Reaches main() (2026-03-21)
- 91,758 functions recompiled (up from 51,658) via branch target splitting
- Game reaches main() initialization: loads 5 modules (SYSUTIL_NP, SPURS, USBD, JPGDEC, NET), registers sysutil callback
- Trampoline system for split-function fallthroughs: 22K conversions, 143K DRAIN_TRAMPOLINE sites
- CRT abort redirect via longjmp — workaround for constructor stack overflows caused by backward branch recursion
- Manual dispatch stubs for mid-function entry points
- Module ID mapping fixes in cellSysmodule
- Root cause identified: lifter backward-branch recursion across split-function boundaries
- Next: implement sys_mmapper_allocate_address, fix lifter backward branch handling

### v0.3.0 — Game Boots (2026-03-15)
- Lifted 51,658 functions to C++ (up from 18,159)
- Added 64-bit instruction lifting: rldicl, rldicr, rldic, rldimi, indexed loads/stores, FP fused multiply-add, FP compare/conversion
- Wired HLE modules with NID-based dispatch (12 modules, 140 handlers)
- Implemented real HLE: sys_initialize_tls, sys_process_exit, cellSysmoduleLoadModule
- Fixed ps3recomp NID computation (16-byte suffix, little-endian)
- Game boots, enters recompiled CRT code, initializes TLS successfully
- 37 MB native executable

### v0.2.0 — Runtime Alignment (2026-03-12)
- Updated to ps3recomp v0.3.1 API
- Added ELF segment loader, recompile pipeline, stubs system
- Aligned CMake build with ps3recomp project template

### v0.1.0 — Binary Analysis (2026-03-10)
- Extracted flOw from PSN package
- Discovered 18,159 functions, resolved 139/140 import NIDs
- Created project structure and toolchain integration

---

*"Eat or be eaten."*
