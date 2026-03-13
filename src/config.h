/*
 * flOw Recomp - Build configuration
 */
#ifndef FLOW_CONFIG_H
#define FLOW_CONFIG_H

/* Game metadata */
#ifndef FLOW_TITLE_ID
#define FLOW_TITLE_ID "NPUA80001"
#endif

#ifndef FLOW_GAME_DIR
#define FLOW_GAME_DIR "game"
#endif

/* Window defaults */
#define FLOW_WINDOW_WIDTH   1920
#define FLOW_WINDOW_HEIGHT  1080
#define FLOW_WINDOW_TITLE   "flOw"

/* ELF entry point (from analysis) */
#define FLOW_ENTRY_POINT    0x846AE0

/* Memory layout */
#define FLOW_MAIN_MEM_SIZE  (256ULL * 1024 * 1024)  /* 256 MB XDR */
#define FLOW_STACK_SIZE     (1 * 1024 * 1024)        /* 1 MB default stack */

/* Threading */
#define FLOW_MAX_PPU_THREADS 64

#endif /* FLOW_CONFIG_H */
