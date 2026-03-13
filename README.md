# flOw — Static Recompilation

> The first native PC port of thatgamecompany's *flOw*, rebuilt from the PS3 binary through static recompilation.

**flOw** (2007) was the debut PlayStation Network title from thatgamecompany — the studio that went on to create *Flower*, *Journey*, and *Sky*. Originally a browser Flash game by Jenova Chen, the PS3 version was rebuilt on Sony's PhyreEngine with 1080p rendering, SIXAXIS tilt controls, and a hypnotic ambient soundtrack. It never received a PC release.

Until now.

This project takes the PS3 `EBOOT.elf` binary, disassembles all 18,159 PowerPC functions, lifts them to C, and links against [ps3recomp](https://github.com/sp00nznet/ps3recomp) — a set of HLE runtime libraries that replace the PS3 operating system with native host implementations. The result is a standalone Windows executable that runs flOw natively, no emulator required.

## Current Status

| Metric | Value |
|---|---|
| Title | flOw |
| Title ID | NPUA80001 |
| Engine | PhyreEngine (Sony) |
| Functions discovered | 18,159 |
| Imported libraries | 12 |
| Imported functions | 140 (139 resolved, 99.3%) |
| Binary size | ~10 MB (ELF) |
| ps3recomp version | v0.3.1 |
| Target | Windows x86-64 (Linux planned) |

### Phase Progress

| Phase | Status | Notes |
|---|---|---|
| PKG extraction | Complete | 553 files extracted from PSN package |
| SELF decryption | Complete | EBOOT.BIN -> EBOOT.elf via RPCS3 + RAP |
| Binary analysis | Complete | 18,159 functions, 12 imported libraries |
| NID resolution | Complete | 139/140 import NIDs mapped to function names |
| PPU disassembly | In Progress | Full PowerPC64 disassembler ready in ps3recomp |
| C lifting | Planned | PPU -> C translation via ppu_lifter |
| Runtime linking | Planned | Link against ps3recomp HLE libraries |
| Graphics backend | Planned | RSX -> D3D12 translation |
| Audio backend | Planned | cellAudio -> WASAPI |
| Native build | Planned | Compile and run on PC |

## Import Map

flOw imports 140 functions from 12 PS3 system libraries. Here's what the game needs and what ps3recomp v0.3.1 provides:

| Library | Functions | ps3recomp Status | Key APIs |
|---|---|---|---|
| **sys_net** | 21 | Complete | Full BSD sockets — socket, bind, connect, send, recv, poll, select |
| **cellGcmSys** | 18 | Complete | GPU init, display buffers, flip, tile/zcull, memory mapping, labels |
| **sys_io** | 17 | Complete | Pad (6), Keyboard (5), Mouse (4) — all three input devices |
| **cellSysutil** | 15 | Complete | Video/audio output config, save data, message dialogs, HDD check |
| **sysPrxForUser** | 15 | Complete | Thread create/exit, lwmutex, TLS init, process exit |
| **cellSpurs** | 14 | Partial | SPU workload management, init/finalize, priorities, wake |
| **sys_fs** | 11 | Complete | File I/O — open, read, write, close, stat, directory ops |
| **sceNp** | 10 | Complete | NP init/term, tickets, presence, basic messaging |
| **cellAudio** | 7 | Complete | Audio port open/close/start/stop, config, init/quit |
| **cellNetCtl** | 5 | Complete | Network init/term, get info, start dialog |
| **cellSysmodule** | 4 | Complete | Module load/unload/init/finalize |
| **cellSync** | 3 | Complete | Barrier init, wait, try-wait |

**Coverage: 11/12 libraries fully implemented, 1 partial (cellSpurs).**

As of ps3recomp v0.3.1, nearly all of flOw's HLE dependencies are complete. The remaining partial module (cellSpurs) covers SPU workload management — PhyreEngine uses SPURS for physics and particle effects on the Cell SPUs. These workloads will need to be either recompiled for the host CPU or stubbed if non-essential to gameplay.

## How It Works

```
+-----------------------------------------------------------+
|                    flOw EBOOT.elf                          |
|              (PowerPC 64-bit, big-endian)                  |
+-----------------------------+-----------------------------+
                              |
                         +----v----+
                         | Analyze |  find_functions.py -> 18,159 functions
                         +----+----+
                              |
                       +------v------+
                       | Disassemble |  ppu_disasm.py -> PowerPC assembly
                       +------+------+
                              |
                         +----v---+
                         |  Lift  |  ppu_lifter.py -> C source code
                         +----+---+
                              |
               +--------------v--------------+
               |    Link & Compile           |
               |                             |
               |  recomp/*.c                 |
               |  + ps3recomp runtime        |---> flow.exe
               |  + graphics backend (D3D12) |
               |  + audio backend (WASAPI)   |
               +--------------+--------------+
```

### The PhyreEngine Challenge

flOw was built on **PhyreEngine**, Sony's first-party game engine. This means:

- **Graphics**: Direct RSX/GCM command buffer submission (not a high-level API). The recompiled code will emit GCM commands that need translation to a modern graphics API.
- **SPU workloads**: PhyreEngine offloads physics and particle work to SPUs via SPURS. These need to be either recompiled for the host CPU or stubbed if non-essential.
- **Shaders**: Cg vertex/fragment programs (`.cgvpo`/`.cgfpo`) that target the RSX's shader ISA. These need conversion to HLSL/SPIR-V.

### Game Data

```
game/
├── PARAM.SFO              # Title metadata
├── ICON0.PNG              # Game icon (320x176)
├── EBOOT.elf              # Decrypted ELF64 (our target)
└── Data/
    ├── Campaigns/         # Level definitions (XML)
    ├── Meshes/            # 3D models (PSSG format)
    ├── Music/             # Soundtrack (MP3)
    ├── Shaders/           # Cg programs
    ├── Sounds/            # Sound effects (.bnk)
    └── Textures/          # Compressed textures (.dds.gz)
```

## Building

### Prerequisites

- Python 3.8+ with packages: `pycryptodome`, `capstone`, `construct`, `tabulate`, `tomli`, `tqdm`
- CMake 3.20+
- A C/C++ compiler (MSVC 2022 / Clang / GCC)
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) (expected at `../ps3/` or set `PS3RECOMP_DIR`)

### Steps

```bash
# Clone
git clone https://github.com/sp00nznet/flow.git
cd flow

# Place your decrypted EBOOT.elf and game data in game/
# (You need a legitimate copy of flOw from PSN)

# Run the recompilation pipeline
python ../ps3/tools/ppu_lifter.py --config config.toml

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run
./build/flow
```

> **Note**: You must supply your own copy of flOw (NPUA80001). This repo contains only the recompilation toolchain integration and runtime code, not any copyrighted game data.

## Project Structure

```
flow/
├── README.md
├── CMakeLists.txt         # Build config (links ps3recomp runtime)
├── config.toml            # Recompiler configuration
├── imports.json           # Resolved import table (140 functions)
├── game/                  # Place EBOOT.elf + Data/ here (not in repo)
├── src/
│   ├── main.cpp           # Entry point, runtime initialization
│   ├── stubs.cpp          # Placeholder function table, game-specific overrides
│   ├── config.h           # Build-time constants
│   └── recomp/            # Recompiled output (generated, not in repo)
├── tools/                 # Pipeline scripts
└── docs/
    └── analysis.md        # Binary analysis notes
```

## Related Projects

- **[ps3recomp](https://github.com/sp00nznet/ps3recomp)** — The PS3 HLE runtime library this project builds against
- **[burnout3](https://github.com/sp00nznet/burnout3)** — Static recomp of Burnout 3: Takedown (PS2/Xbox)
- **[pcrecomp](https://github.com/sp00nznet/pcrecomp)** — PC game recompilation framework
- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** — The project that proved static recompilation works at scale
- **[RPCS3](https://github.com/RPCS3/rpcs3)** — PS3 emulator whose HLE research makes this possible

## Legal

This project does not contain any proprietary Sony code, game binaries, encryption keys, or copyrighted game assets. It is a clean-room reimplementation of PS3 system libraries paired with automated binary translation tools. Users must supply their own legally obtained copy of flOw.

## Changelog

### v0.2.0 — Runtime Alignment (2026-03-12)
- Updated to ps3recomp v0.3.1 API (C++ namespace pattern)
- Added stubs.cpp with placeholder function table and game-specific overrides
- Added config.toml for recompiler pipeline configuration
- Updated import map: 11/12 libraries now fully implemented in ps3recomp
- Added bcrypt to Windows link libraries (required by ps3recomp v0.3.x)
- Aligned CMake build system with ps3recomp project template

### v0.1.0 — Binary Analysis (2026-03-10)
- Extracted 553 files from flOw PSN package (NPUA80001)
- Decrypted SELF -> ELF64 using RPCS3
- Discovered 18,159 functions via automated analysis
- Resolved 139/140 import NIDs (99.3%) across 12 libraries
- Mapped complete dependency graph against ps3recomp modules
- Created project structure and toolchain integration

---

*"Eat or be eaten."*
