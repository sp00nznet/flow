# flOw SPU workstream — scope

**Why:** RPCS3-oracle analysis (2026-06-18) proved flOw's boot is SPU/SPURS-gated.
The PPU init creates SPURS workloads + SPU thread groups, pushes work via `cellSync`
lock-free queues, and **waits on SPU completion** (`cellSpursWorkloadFlagReceiver`)
before the resource manager (`func_000FC1F0`) loads `Classes.xml`/`first.xml` (the type
system + boot scene). Our PPU-only recomp never runs the SPU side, so it never reaches
the loader → no scene → no entities. Real engine-driven rendering requires running the
SPU half. The committed host-side scene-builder stays the visible stand-in meanwhile.

## Inventory

| Thing | State |
|---|---|
| flOw embedded SPU programs | **60 SPU ELFs in EBOOT, ~30 unique**, 1 KB–637 KB, ~1 MB total |
| ps3recomp `spu_lifter.py` / `spu_disasm.py` | exist (29 KB / 23 KB), exercised on `cri_job` (see `fw_spu/cri_job_lifted*`) |
| `runtime/spu/` | real: `spu_channels.c`, `spu_context.h`, `spu_dma.h`, `spu_helpers.h`, `tests/` |
| `libs/sync/cellSync.c` | **real LFQueue** (113 impl refs) — the PPU↔SPU queue primitives mostly exist |
| `libs/spurs/cellSpurs.c` | **39 funcs, all stubbed to `CELL_OK`** — no real workload/SPU execution |
| `libs/spurs/cellFiber.c` | stub |

## Gaps to close

1. **Extract + lift flOw's ~30 SPU programs.** `spu_lifter` exists but the embedded-ELF
   unwrap step needs work (prior note: `find_spu_functions` returned 0 on raw embedded
   ELFs — it decoded the ELF header as code). Then lift → C → link → run.
2. **Real `cellSpurs` runtime.** Workload/taskset management, SPU thread-group execution,
   and the **workload-flag signaling** the PPU waits on. Currently all stubs.
3. **PPU↔SPU bridge.** Dispatch lifted SPU programs against shared memory (local store ↔
   256 MB XDR via DMA), driven by the recompiled PPU's SPURS calls, with `cellSync`
   LFQueue push/pop + workload-flag completion wired end to end.
4. **Boot-path subset.** Identify WHICH SPU job(s) gate the scene load — not all 30 are
   needed to boot; most are runtime jobs (particle sim, culling).

## Two approaches

- **A — HLE-fake the coordination (cheap probe, ~hours).** Make `cellSpurs` workload-flag
  + `cellSync` LFQueue HLE report "work complete" without running any SPU. Rebuild, see if
  the boot reaches `func_000FC1F0`. *Decisive cheaply:* if the boot-path SPU work is a pure
  handshake (no essential data), the PPU advances and we may reach the scene load PPU-only.
  Risk: if the SPU produces data the PPU consumes, the PPU proceeds with garbage → crash.
- **B — Real SPU execution (full, weeks).** Lift the SPU programs + wire real `cellSpurs` +
  DMA + the PPU↔SPU model. Correct; required for actual gameplay.

## Recommended staged plan

1. **Probe A first** — HLE-complete the SPURS workload-flag/LFQueue wait, rebuild, check
   whether `func_000FC1F0` is reached and what the PPU does with the (faked) result. One
   build cycle; tells us if the gate is pure-coordination or data-dependent.
2. **Stage B1 (if data-dependent)** — lift only the **boot-path** SPU program(s) (SPURS
   bootstrap + the early job), wire DMA + the LFQueue/flag handshake, get the scene to load
   with real SPU output. Days–week.
3. **Stage B2** — lift the remaining runtime SPU jobs (particle/scene processing) for live
   gameplay. Weeks; this is where the organism + particles become engine-driven, not
   host-synthesized.

## First concrete step

Run **Probe A**: in `libs/spurs/cellSpurs.c` + `libs/sync/cellSync.c`, make the
workload-flag receiver and LFQueue pop return "ready/complete" immediately, rebuild flOw,
and trace whether `func_000FC1F0` (the scene-config loader) is finally reached and whether
the PPU then opens `Classes.xml`. That single experiment partitions the entire workstream
into "coordination-only (cheap)" vs "needs real SPU (B1/B2)".
