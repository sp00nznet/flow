/*
 * flow_level.h — Loader for flOw level XML data.
 *
 * Parses CLevel/CSnakeFactory/CFoodFactory/CParticleHerd from the
 * game's Data/Campaigns/<C>/v1/C<C>_L<N>_v1.xml files so the renderer
 * can drive its placeholder geometry from real game data instead of
 * hardcoded literals.
 *
 * Design notes:
 *  - No external XML dependency. flOw level XMLs are flat (one element
 *    per line, attribute-style), so a tiny attribute scanner is enough.
 *  - Numeric ranges in the XML use bracket syntax: NumObjects="[16-18]"
 *    or NumNour="[1]" — we take the lower bound (deterministic seed).
 *  - All values are best-effort; missing fields fall back to defaults
 *    that match Campaign_1/v1/C1_L3_v1.xml so the existing scene still
 *    renders if the file is absent.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOW_LEVEL_MAX_FOODS     8
#define FLOW_LEVEL_MAX_CREATURES 8

typedef struct FlowFoodFactory {
    int   num_objects;          /* lower bound of [m-n] range */
    char  food_type[32];        /* e.g. "CFoodBasicSegment" */
    int   num_nour;
} FlowFoodFactory;

/* CSnake/CJelly/CManta/CHunter share the same factory shape — we read
 * them all into one array and tag with the kind. */
typedef enum FlowCreatureKind {
    FLOW_CREATURE_SNAKE  = 0,
    FLOW_CREATURE_JELLY  = 1,
    FLOW_CREATURE_MANTA  = 2,
    FLOW_CREATURE_HUNTER = 3,
} FlowCreatureKind;

typedef struct FlowCreatureFactory {
    int   kind;                 /* FlowCreatureKind */
    int   num_objects;          /* how many of this creature */
    int   num_segs;
    float joint_dist;
    float radius;
    float max_speed;
} FlowCreatureFactory;

typedef struct FlowLevel {
    int   loaded;               /* 1 if parsed successfully */
    char  source_path[256];     /* file we actually loaded */

    /* Background gradient (RGBA, 0..1) */
    float bot_color[4];
    float mid_color[4];
    float top_color[4];
    float radius;
    float glow;

    /* Particles — herd of Dot/Dot3 etc. */
    int   particle_count;       /* sum of all CParticleHerd Number */
    char  particle_species[32]; /* primary species (last non-zero) */

    /* Snake (single factory in C1_L3) — kept for back-compat with the
     * earlier renderer; mirror of creatures[0] when kind==SNAKE. */
    int   snake_num_segs;       /* lower bound */
    float snake_joint_dist;
    float snake_radius;
    float snake_max_speed;

    /* All creature factories (snake/jelly/manta/hunter) the level declares. */
    int                  creature_count;
    FlowCreatureFactory  creatures[FLOW_LEVEL_MAX_CREATURES];

    /* Food factories (concatenated across <CFoodFactory> entries) */
    int             food_factory_count;
    FlowFoodFactory foods[FLOW_LEVEL_MAX_FOODS];
    int             total_food;        /* sum of food num_objects */
} FlowLevel;

extern FlowLevel g_flow_level;

/* Reset to built-in defaults (Level 3 colors). */
void flow_level_reset_defaults(void);

/* Try to load the named level XML. `level_path` may be relative; the
 * loader probes a few candidate roots:
 *   - level_path as-is
 *   - "<game_root>/<level_path>" using FLOW_GAME_DIR
 *   - "extracted/USRDIR/<level_path>"
 * Returns 1 on success, 0 on failure (defaults remain in place). */
int  flow_level_load(const char* level_path);

/* Convenience: load Campaign C, Level N from the standard layout. */
int  flow_level_load_campaign(int campaign, int level);

#ifdef __cplusplus
}
#endif
