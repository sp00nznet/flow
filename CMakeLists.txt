# flОw (NPUA80001, thatgamecompany / PhyreEngine) — clean PPU boot harness.
#
# Proper static-recomp boot: wires the fresh lifter output (src/recomp_v2) to the
# generic first-boot harness (runtime/ppu/tests/boot_main.cpp): load the real
# EBOOT -> register lifted functions + HLE NID handlers -> run real boot code from
# _start through the game's own CRT/ctors. NO bypass/redirect/inject machinery
# (that was the old CMakeLists.hacked.bak). Real game data loads via the cellFs
# VFS (PS3_VFS_ROOT). Optional real cellSpurs via libsre (YDKJ_LIBSRE=1).
#
# Toolchain: clang-cl (boot harness uses __builtin_bswap + weak symbols MSVC lacks).
#   cmake -S . -B build_boot -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build_boot
#   ./build_boot/flow_boot game/EBOOT.elf
cmake_minimum_required(VERSION 3.20)
project(flow_boot CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

# ps3-draw worktree, branch `ps3recomp` (supersedes draw-engine): caner's PPU/SPU
# lifter correctness + NID fixes, sagemono's RSX->D3D12 draw engine (per-draw VP/FP
# programs, RTT/MRT, viewport-Z), the SPU interpreter/self-mod path, sys_fs
# transient-open retries, serialized vblank/flip delivery, and the merged `ydkj`
# SPURS work (7-arg cellSpursCreateTask, BE cellSpursEventFlag, taskset context).
# Relift required to pick up the lifter fixes -- see src/recomp_v5.
set(PS3RECOMP_DIR "D:/recomp/ps3-draw" CACHE PATH "ps3recomp repo root")
set(PS3RECOMP_RUNTIME_LIB "${PS3RECOMP_DIR}/build/Release/ps3recomp_runtime.lib"
    CACHE FILEPATH "prebuilt runtime library")

# Lifted game code. RECOMP_SRC_DIR selects the lift: recomp_v5 (find_functions +
# gap-filled list + jump-table-base fix -- drives the app loop) by default;
# -DRECOMP_SRC_DIR=src/recomp_v2 builds the old lift for isolation.
set(RECOMP_SRC_DIR "src/recomp_v5" CACHE STRING "lifted-source dir")
file(GLOB RECOMP_SOURCES "${RECOMP_SRC_DIR}/*.c" "${RECOMP_SRC_DIR}/*.cpp")
file(GLOB GEN_SOURCES    "src/gen/*.cpp")

# Real system-PRX module: libsre (Sony cellSpurs/cellSync), lifted + namespaced
# (libsre_* prefix). The runtime PRX loader places the relocated image into guest
# RAM and registers its exports so cellSpurs imports dispatch into real recompiled
# Sony code (gated at runtime by YDKJ_LIBSRE=1; default falls back to HLE stubs).
# The lifted .c uses C++ constructs -> force C++.
file(GLOB PRX_MODULE_SOURCES
    "prx/libsre_ns/ppu_recomp.c"
    "prx/libsre_ns/libsre_exports.c"
    "prx/libsre_ns/libsre_externs.cpp"
)
if(PRX_MODULE_SOURCES)
    set_source_files_properties(${PRX_MODULE_SOURCES} PROPERTIES LANGUAGE CXX)
endif()

# Firmware SPU kernel (libsre's cellSpurs SPU program: kernel/policy/task layers).
# The runtime SPU dispatcher (runtime/spu/spu_workload.c) hard-references the
# policy entry tsp_spu_func_00000A00, so the title must provide these lifts.
# Firmware-version-close, reused from the YDKJ sister-project (same as libsre).
file(GLOB SPU_KERNEL_SOURCES
    "prx/spu_kernel/lift_a/spu_recomp.c"
    "prx/spu_kernel/lift_pol/spu_recomp.c"
    "prx/spu_kernel/lift_tsp/spu_recomp.c"
)

# flОw's OWN SPURS workload modules — the SPU programs the game hands to
# cellSpursAddWorkload (pm=0x10062600/0x10063900/0x10067400/0x1010EC00). These are
# RAW SPU images (no ELF header), which is why the old ELF-magic extractor missed
# them and the 60 lifted `spu_extract/flow_spu_*` images never covered them. Lifted
# with distinct symbol prefixes (wl0_..wl3_) so all four co-link.
file(GLOB FLOW_SPU_WORKLOAD_SOURCES
    "spu_extract/workloads/flow_wl_0_62600_lifted/spu_recomp.c"
    "spu_extract/workloads/flow_wl_1_63900_lifted/spu_recomp.c"
    "spu_extract/workloads/flow_wl_2_67400_lifted/spu_recomp.c"
    "spu_extract/workloads/flow_wl_3_10EC00_lifted/spu_recomp.c"
)

add_executable(flow_boot
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"   # generic boot harness
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
    "${PS3RECOMP_DIR}/runtime/syscalls/sys_fs.c"         # sys_fs syscalls (override prebuilt for instrumentation)
    "${PS3RECOMP_DIR}/libs/system/cellSaveData.c"        # override prebuilt: guest-EA dirName translation fix
    "${PS3RECOMP_DIR}/libs/video/cellVideoOut.c"         # override prebuilt: GetState.state ENABLED(0) not PREPARING(2) -> PhyreEngine builds display config
    "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c"          # real-PRX loader (libsre)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/malloc_hle.cpp"     # guest allocator HLE (heap init)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/prx_init.cpp"       # libsre load hook (real cellSpurs)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flow_spurs_kernel.c"  # run the lifted SPURS SPU kernel
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flow_extra_nids.cpp"  # wire missing HLE NIDs (cellGcmSetFlip, cellPadGetInfo, ...)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flow_run_ctors.cpp"   # explicit static-ctor runner (lifted .init_array skips some)
    "${CMAKE_CURRENT_SOURCE_DIR}/src/flow_spu_workloads.c" # RUN flОw's SPURS workloads (HLE records but never executes them)
    # Override the prebuilt lib's copy: spu_dma.h gained the policy-image registry
    # (switch SPU image_id when the kernel DMAs a policy to LS 0xA00) and this TU
    # inlines the MFC path. Without recompiling it here the lib's stale copy runs.
    # (spu_workload.c is NOT overridden — it has a type conflict in this include
    # context; the registry is implemented in flow_spu_workloads.c instead.)
    "${PS3RECOMP_DIR}/runtime/spu/spu_channels.c"
    ${GEN_SOURCES}
    ${RECOMP_SOURCES}
    ${SPU_KERNEL_SOURCES}
    ${FLOW_SPU_WORKLOAD_SOURCES}
    ${PRX_MODULE_SOURCES}
    # libsre (real cellSpurs) NOW INCLUDED — PhyreEngine PSSG needs the real CellSpurs
    # struct layout (HLE simplified struct can't satisfy it). Gate runtime: YDKJ_LIBSRE=1. — libsre links against
    # the SPU kernel lift (tsp/pol/a layers) flОw doesn't have yet. First boot uses
    # the weak no-op ps3_load_prx_modules -> cellSpurs falls to runtime HLE stubs +
    # the SPU group tracker. Re-add ${PRX_MODULE_SOURCES} + prx_init.cpp + the SPU
    # kernel sources once we lift them (then enable with YDKJ_LIBSRE=1).
)

# Every libs/* subdir (the generated NID table #includes each module's header).
file(GLOB LIB_INC_DIRS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")

target_include_directories(flow_boot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/compat"             # <dirent.h>/<unistd.h> Win shims
    "${CMAKE_CURRENT_SOURCE_DIR}/${RECOMP_SRC_DIR}"      # ppu_recomp.h
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/prx/libsre_ns"          # libsre ppu_recomp.h
    ${LIB_INC_DIRS}
)

target_link_libraries(flow_boot PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
if(WIN32)
    target_link_libraries(flow_boot PRIVATE ws2_32 bcrypt d3d12 dxgi dxguid d3dcompiler)
    # Deep recompiled call chains nest on the host stack; reserve 256 MB.
    target_link_options(flow_boot PRIVATE /STACK:268435456 /MAP:flow.map)
endif()
