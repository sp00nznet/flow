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

## Probe A — RESULT (2026-06-18, answered without a build)

Faking the SPU coordination is **moot**: our recomp's boot trace shows ONLY the pre-seed
`cellSpursInitialize` — it never reaches `cellSpursWorkloadFlagReceiver`, the `cellSync`
LFQueues, `sys_spu_thread_group_*`, or the real SPURS setup. **The divergence is upstream
of the coordination, not at it.**

What the working RPCS3 boot does that ours does NOT: at ~0:00:48 (right before the scene
load) a tight burst — real SPURS setup (`cellSpursInitializeWithAttribute` + `CreateTaskset`
+ `AddWorkload`, not the plain `Initialize` our pre-seed uses), **330 `sys_fs_open`** (the
whole asset-loading phase), 86 `sys_memory_allocate`, `sys_event_queue/port` setup, 1
`sys_spu_initialize`. Our recomp does **0** `cellFs` calls and **0** real SPURS setup.

**Root reframe:** our recomp runs flOw's hand-written **bypass loop** (the func_000C858C
injection) instead of the natural engine-run that loads assets + drives SPU. So the gap
isn't one gate — it's an **entire boot phase we never enter**. The host-side bypasses
(scene-builder, vtable-forcing, SPURS pre-seed) keep flOw "alive"/rendering but on a
dead-end path that diverges from the natural boot well before SPURS/asset-load.

## Revised scope (bigger than one probe)

Real engine-driven flOw needs THREE entangled pieces, not just "run the SPU":
1. **Un-bypass the PPU boot** — let the natural engine-run reach the SPURS/asset-load phase
   (now plausible post func_table-fix; the bypass was built for the broken-ctor era). This
   is its own PPU-debugging effort: find why game-main's natural path doesn't enter the
   ~48s asset/SPURS phase.
2. **Serve file I/O** — real `cellFs` backed by `extracted/USRDIR/Data/` so the 330+ asset
   opens (Classes.xml/first.xml/textures/sounds) succeed.
3. **Run the SPU + wire coordination** — lift the ~30 SPU programs (spu_lifter), real
   `cellSpurs` workload/taskset + `cellSync` LFQueue + workload-flag handshake.

Estimate: multi-week re-architecture away from the host-synthesis approach. The committed
host-side scene-builder remains the pragmatic visible stand-in.

## Recommended next concrete step (cheap, build-free first)

Use the RPCS3 oracle once more to find the FIRST divergence in the PPU boot: get a richer
RPCS3 trace (raise cellFs/cellSpurs/PPU log levels), identify the function that drives the
~48s asset/SPURS phase, and locate the equivalent point in our recomp to see exactly where
the bypass takes over vs where the natural path continues. That pinpoints piece #1 (the
PPU un-bypass) before committing to the SPU lift (#3).
