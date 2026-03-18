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
| Functions discovered | 68,376 (OPD + heuristic + branch targets) |
| Functions lifted to C | 68,376 (100%) |
| Imported libraries | 12 |
| Imported functions | 140/140 resolved (100%) |
| Binary size | ~10 MB (ELF) → ~40 MB (exe) |
| HLE bridges | 7/12 real (cellSysutil, cellGcmSys, cellAudio, cellPad, cellFs, cellSysmodule, sysPrxForUser) |
| Remaining TODOs | ~24,660 (mostly VMX/AltiVec) |
| ps3recomp version | v0.4.0 |
| Target | Windows x86-64 (Linux planned) |

### Phase Progress

| Phase | Status | Notes |
|---|---|---|
| PKG extraction | **Complete** | 553 files extracted from PSN package |
| SELF decryption | **Complete** | EBOOT.BIN → EBOOT.elf via RPCS3 + RAP |
| Binary analysis | **Complete** | 51,658 functions via OPD + heuristic analysis |
| NID resolution | **Complete** | 140/140 import NIDs mapped to function names |
| ELF structural analysis | **Complete** | Segments, sections, OPD, TOC, memory map |
| PPU lifting | **Complete** | 68,376 functions lifted to C++ |
| Runtime linking | **Complete** | Builds and links against ps3recomp runtime |
| HLE module registration | **Complete** | 12 modules, 7 with real HLE bridges |
| Game boot | **Working** | CRT entry → TLS → mutex init → malloc |
| Graphics backend | Planned | RSX → D3D12 translation |
| Audio backend | **Wired** | cellAudio → WASAPI via ps3recomp |
| Input backend | **Wired** | cellPad → XInput via ps3recomp |
| Full gameplay | In Progress | Debugging CRT startup and game init |

### What Works Now

- **68,376 PPC64 functions** lifted to native C++ (extended from 51K with branch target analysis)
- **7 HLE modules with real bridges** — proper PPC64 ABI parameter extraction, BE struct output, host OS backends
- **Real WASAPI audio** via ps3recomp's cellAudio mixing thread
- **Real XInput gamepad** via ps3recomp's cellPad backend
- **Real filesystem I/O** via ps3recomp's cellFs path translation
- **Real lightweight mutexes** via CRITICAL_SECTION-backed sysPrxForUser
- **CRT startup progresses** through TLS init → mutex creation → malloc
- **All ELF segments** loaded into 4 GB virtual memory (text, data, rodata, BSS, RSX region)
- **LV2 syscall dispatch** with sys_tty_write for CRT debug output
- **NID-based HLE dispatch** for all 140 imports, with TOC save fix for PPC64 ABI compliance

## Import Map

flOw imports 140 functions from 12 PS3 system libraries:

| Library | Functions | HLE Status | Key APIs |
|---|---|---|---|
| **sys_net** | 21 | Stubbed | BSD sockets — socket, bind, connect, send, recv, poll |
| **cellGcmSys** | 18 | Stubbed | GPU init, display buffers, flip, tile/zcull, memory mapping |
| **sys_io** | 17 | Stubbed | Pad, Keyboard, Mouse input |
| **cellSysutil** | 15 | Stubbed | Video/audio config, save data, message dialogs |
| **sysPrxForUser** | 15 | **Partial** | TLS init, thread create/exit, lwmutex, process exit |
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
                  | Analyze (18K+37K OPD) |  51,658 functions
                  +-----------+-----------+
                              |
                    +---------v---------+
                    | Lift (ppu_lifter) |  156 MB of C++ code
                    +---------+---------+
                              |
               +--------------v--------------+
               |    Link & Compile           |
               |                             |
               |  ppu_recomp.cpp (156 MB)    |
               |  + import_stubs.cpp         |
               |  + hle_modules.cpp          |
               |  + vm_bridge.cpp            |---> flow.exe (37 MB)
               |  + ps3recomp runtime        |
               |  + elf_loader.cpp           |
               +--------------+--------------+
```

### Architecture

- **vm_bridge.cpp** — Bridges lifter's uint64_t memory API to ps3recomp's uint32_t big-endian VM
- **hle_modules.cpp** — Registers 12 PS3 modules with NID→handler mappings for HLE dispatch
- **import_stubs.cpp** — 140 import stub functions that resolve NIDs at runtime
- **elf_loader.cpp** — Loads PT_LOAD segments from EBOOT.elf into virtual memory
- **ppu_recomp.cpp** — 51,658 lifted C++ functions (auto-generated)
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

# Run the recompilation pipeline (takes ~90 min for 51K functions)
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
│   └── recomp/                 # Generated recompiled code (not in repo)
│       ├── ppu_recomp.cpp      # 51,658 lifted functions (156 MB)
│       ├── ppu_recomp.h        # Context struct + declarations
│       ├── func_table.cpp      # Guest→host function mapping
│       ├── import_stubs.cpp    # 140 NID-dispatching import stubs
│       └── missing_stubs.cpp   # Empty stubs for unlifted call targets
├── tools/
│   └── recompile.py            # Master recompilation pipeline
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
