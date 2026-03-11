# flOw Binary Analysis

## EBOOT.elf Overview

| Property | Value |
|---|---|
| Format | ELF64, big-endian, PowerPC64 |
| File size | 10,140,464 bytes |
| Entry point | 0x846AE0 |
| Program headers | 8 |
| Section headers | 34 |
| Functions | 18,159 (auto-discovered) |

## ELF Segments

The binary has 8 program headers. Key segments:

- **PT_LOAD (code)**: Contains the main executable code and read-only data
- **PT_LOAD (data)**: Writable data, BSS
- **PT_TLS**: Thread-local storage template
- **PT_LOOS+2**: SCE process param (points to `sys_process_prx_param`)

## Import Libraries

12 libraries, 140 function imports. See `imports.json` for the complete NID mapping.

### Library Dependency Graph

```
flOw EBOOT.elf
├── cellGcmSys (18)      — RSX GPU interface
├── cellSysutil (15)     — System utilities, save data, video/audio config
├── sysPrxForUser (15)   — Thread management, lwmutex, process control
├── sys_net (21)         — BSD socket networking
├── sys_io (17)          — Gamepad, keyboard, mouse input
├── cellSpurs (14)       — SPU workload management
├── sys_fs (11)          — Filesystem I/O
├── sceNp (10)           — PlayStation Network
├── cellAudio (7)        — Audio output
├── cellNetCtl (5)       — Network control
├── cellSysmodule (4)    — Module loader
└── cellSync (3)         — Barrier synchronization
```

## PhyreEngine Observations

The binary uses Sony's PhyreEngine, evidenced by:

1. **PSSG mesh format** in `Data/Meshes/` — PhyreEngine's proprietary scene graph
2. **Cg shaders** in `Data/Shaders/` — compiled for RSX (`.cgvpo`/`.cgfpo`)
3. **Direct GCM usage** — 18 cellGcmSys imports for low-level RSX control
4. **SPURS workloads** — 14 cellSpurs imports for SPU task management
5. **Campaign XML** — PhyreEngine's data-driven level format

## Function Size Distribution

From the 18,159 discovered functions:
- Median function size: ~64 bytes (16 instructions)
- Largest functions: likely PhyreEngine rendering loops
- Many small leaf functions: typical of C++ with inlining

## Key Addresses

| Symbol | Address | Notes |
|---|---|---|
| Entry point | 0x846AE0 | `_start` / OPD entry |
| lib.stub names | 0x808150 | Library name string table |
| sys_process_prx_param | 0x818678 | PRX param structure |
| Import stub table | 0x818444 | 12 entries x 0x2C bytes |
| FNID arrays | 0x818208+ | NID values for each library |
| Function stubs | 0x827768+ | Branch stubs for imports |

## Unresolved NID

One NID remains unresolved:

- **Library**: sceNp
- **NID**: `e7dcd3b4`
- **Likely**: An obscure sceNp utility function, possibly `sceNpBasicSetPresenceDetails` or similar

This function can be safely stubbed (return CELL_OK) for initial testing.
