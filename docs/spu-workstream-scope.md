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

## Divergence diff — the exact missing-syscall checklist (2026-06-18)

Set-difference of distinct HLE/syscall calls: WORKING boot makes 62, ours 43. Everything
the working boot does that ours does ZERO of is one coherent subsystem — the SPU/SPURS/PRX
bring-up:

- **PRX loading:** `_sys_prx_load_module`, `_sys_prx_start_module`,
  `_sys_prx_get_module_id_by_name` (dynamically loads libsre.sprx = SPURS runtime, etc.)
- **SPU threads:** `sys_spu_initialize`, `sys_spu_thread_group_create`,
  `sys_spu_thread_initialize`
- **PPU↔SPU events:** `sys_event_queue_create`, `sys_event_port_create`,
  `sys_event_port_connect_local`, `sys_spu_thread_group_connect_event(_all_threads)`
- **Workers/mem:** `_sys_ppu_thread_create`, `sys_memory_container_create/destroy`
- **Real SPURS init:** `cellSpursInitializeWithAttribute` + `CreateTaskset` + `AddWorkload`
  (ours only ever does the injected pre-seed `cellSpursInitialize`)

Our recomp makes NONE of these. Two compounding reasons: (1) it runs flOw's bypass loop, not
the natural engine-run that performs the bring-up; (2) even the cellSpurs entry points are
HLE-stubbed to `CELL_OK`, so they never issue the underlying sys_spu/sys_event syscalls.

This is the concrete build checklist for the SPU bring-up (piece #3), and it confirms pieces
#1 (un-bypass so the engine reaches these calls) and #2 (cellFs) must land too — the three
are entangled and must come together; no single one unblocks alone. Realistic next move is
to prototype the bring-up in isolation (real cellSpurs that issues sys_spu_thread_group_create
+ runs one lifted SPU program against shared LS↔XDR + signals via an event queue) as a unit
test under runtime/spu/tests, decoupled from the flOw boot, then integrate.

## SPU-lifting FEASIBILITY — VALIDATED on flOw's SPU code (2026-06-18)

Extracted one of flOw's 60 embedded SPU ELFs (#3 @ EBOOT file-off 0x8D6F00) and ran the
ps3recomp SPU pipeline end to end — **it works**:
- `find_spu_functions`: text 0x80–0xA80 (640 instrs), 1 function detected, **97% coverage**.
- `spu_lifter --auto-functions`: **639/640 instructions lifted to valid C** (128-bit GPRs,
  `spu_ori/il/a/ai/shlqbyi`, `spu_ls_read128/write128` local-store DMA). **Only 1 unsupported
  op: `frest`** (FP reciprocal estimate — trivial to add).

So piece #3's hardest sub-part — recompiling the SPU ISA — is largely DONE and proven on
flOw. Concrete tooling gaps found:
1. **Embedded-ELF extraction:** sizing must use section *content* extents
   (`max(sh_offset+sh_size)` over SHT≠NOBITS), not the section-header-table end, or `.symtab`
   gets truncated and `read_symbols` crashes. Bake a flOw SPU-extractor that splits the
   EBOOT's ~30 unique SPU ELFs.
2. Add `frest` (+ likely `frsqest`/other estimate ops) to `spu_lifter`.

The remaining effort is NOT the lifting — it's the **runtime integration**: real `cellSpurs`
(issue the sys_spu/sys_event syscalls from the checklist), the LS↔XDR DMA + channel/event
plumbing (`runtime/spu/` has the primitives), the LFQueue/workload-flag PPU↔SPU handshake,
and getting the PPU off the bypass so it drives all this. That integration is the multi-week
core; the SPU recompiler itself is ready.

## Bring-up prototype — BUILT + PASSING (2026-06-18, ps3recomp commit cf478f7)

Built the first integration unit (`runtime/spu/tests/gen_test_bringup.py` +
`test_bringup_main.c`): a hand-assembled SPU program lifted to C (100% coverage) that
writes a result into shared "main memory" via a DMA PUT, then signals the PPU via the
outbound mailbox; the host harness verifies BOTH halves. Passes via `run_tests.sh` (5/5).
This proves the core PPU↔SPU coordination handshake — SPU job produces shared-memory output
AND signals completion — decoupled from flOw's boot.

**Next concrete layers (build on this unit):**
1. Wrap the mailbox signal in a **`sys_event_queue` + `sys_spu_thread_group_connect_event`**
   path (flOw's actual completion mechanism) — minimal host harness, no game.
2. Add a **`cellSync` LFQueue** push/pop round-trip (PPU enqueues work, SPU dequeues +
   processes + enqueues result) — the cellSync impl already exists; wire it to the SPU side.
3. Stand up a minimal **`cellSpurs` taskset** that actually issues `sys_spu_thread_group_create`
   and runs a lifted job (replace the all-`CELL_OK` stubs).
Then integrate against flOw (piece #1 un-bypass + piece #2 cellFs).
