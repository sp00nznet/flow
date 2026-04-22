/*
 * flow_level.cpp — Implementation of the level XML loader.
 *
 * The flOw level XML is hand-edited and has a very small subset of XML:
 *   - Elements per line (or with multi-line attribute lists)
 *   - Attributes use either plain values or bracketed ranges
 *   - Tuple values are parenthesised: "(0, 0.1, 0.3, 1)"
 *
 * Rather than pulling in a full XML library we just scan the buffer for
 * the elements we care about and extract attributes by name.
 */

#include "flow_level.h"
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

extern "C" FlowLevel g_flow_level;
FlowLevel g_flow_level = {};

/* ---------------------------------------------------------------------------
 * Defaults — match Campaign_1/v1/C1_L3_v1.xml so a missing file is harmless.
 * -----------------------------------------------------------------------*/

void flow_level_reset_defaults(void)
{
    FlowLevel& L = g_flow_level;
    memset(&L, 0, sizeof(L));
    L.loaded = 0;
    L.source_path[0] = '\0';

    L.bot_color[0]=0.00f; L.bot_color[1]=0.10f; L.bot_color[2]=0.30f; L.bot_color[3]=1.0f;
    L.mid_color[0]=0.00f; L.mid_color[1]=0.30f; L.mid_color[2]=0.70f; L.mid_color[3]=1.0f;
    L.top_color[0]=0.60f; L.top_color[1]=0.60f; L.top_color[2]=0.80f; L.top_color[3]=1.0f;
    L.radius           = 475.0f;
    L.glow             = 2.7f;

    L.particle_count   = 1800;
    snprintf(L.particle_species, sizeof(L.particle_species), "Dot3");

    L.snake_num_segs   = 8;
    L.snake_joint_dist = 13.5f;
    L.snake_radius     = 13.5f;
    L.snake_max_speed  = 150.0f;

    /* Default food: 16 BasicSegments */
    L.food_factory_count = 1;
    L.foods[0].num_objects = 16;
    snprintf(L.foods[0].food_type, sizeof(L.foods[0].food_type), "CFoodBasicSegment");
    L.foods[0].num_nour    = 1;
    L.total_food = 16;
}

/* ---------------------------------------------------------------------------
 * Attribute helpers
 * -----------------------------------------------------------------------*/

/* Locate `Attr="..."` within [start, end) starting from `from`, returning a
 * pointer to the opening quote, or NULL. Match is whole-word — i.e. the
 * character before `Attr` must be whitespace or '<' so "NumNour" doesn't
 * accidentally match inside "MyNumNour". */
static const char* find_attr(const char* from, const char* end,
                             const char* attr)
{
    size_t alen = strlen(attr);
    for (const char* p = from; p && p + alen + 2 < end; ) {
        const char* hit = strstr(p, attr);
        if (!hit || hit + alen + 2 >= end) return NULL;
        char prev = (hit > from) ? *(hit - 1) : ' ';
        char next = *(hit + alen);
        if ((prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
             prev == '<') &&
            (next == '=' || next == ' ' || next == '\t')) {
            const char* eq = hit + alen;
            while (eq < end && (*eq == ' ' || *eq == '\t')) eq++;
            if (eq < end && *eq == '=') {
                eq++;
                while (eq < end && (*eq == ' ' || *eq == '\t')) eq++;
                if (eq < end && *eq == '"') return eq;
            }
        }
        p = hit + alen;
    }
    return NULL;
}

/* Read an attribute string into `out` (size obytes). Returns 1 on success. */
static int attr_str(const char* base, const char* end, const char* attr,
                    char* out, size_t obytes)
{
    const char* q = find_attr(base, end, attr);
    if (!q) return 0;
    q++;
    const char* eq = (const char*)memchr(q, '"', end - q);
    if (!eq) return 0;
    size_t n = (size_t)(eq - q);
    if (n >= obytes) n = obytes - 1;
    memcpy(out, q, n);
    out[n] = '\0';
    return 1;
}

/* Parse "[m-n]" or "[v]" or plain "v". Returns the lower bound as int. */
static int parse_range_int(const char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '[') s++;
    return atoi(s);
}

static float parse_range_float(const char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '[') s++;
    return (float)atof(s);
}

/* Parse "(r,g,b,a)" into 4 floats; missing components default to 0/1. */
static void parse_rgba(const char* s, float out[4])
{
    out[0]=out[1]=out[2]=0.0f; out[3]=1.0f;
    while (*s == ' ' || *s == '\t' || *s == '(') s++;
    int idx = 0;
    char buf[32];
    while (*s && idx < 4) {
        size_t n = 0;
        while (*s && *s != ',' && *s != ')' && n < sizeof(buf) - 1) {
            buf[n++] = *s++;
        }
        buf[n] = '\0';
        out[idx++] = (float)atof(buf);
        if (*s == ',' || *s == ' ') { s++; }
        else if (*s == ')' || *s == '\0') break;
    }
}

/* Locate the first occurrence of element `<Name` (e.g. "<CSnakeFactory") and
 * return [pointer, end-of-attributes). End is the closing '>' of the open
 * tag. Returns 1 on success. */
static int find_element(const char* buf, size_t len, const char* tag,
                        const char** out_attr_begin,
                        const char** out_attr_end,
                        const char** out_search_continue)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "<%s", tag);
    const char* end = buf + len;
    const char* hit = strstr(buf, needle);
    if (!hit) return 0;
    const char* after = hit + strlen(needle);
    /* Make sure the element name terminates here (next char is space/>/end). */
    if (after >= end ||
        (*after != ' ' && *after != '\t' && *after != '\n' && *after != '\r' &&
         *after != '>' && *after != '/')) {
        return 0;
    }
    const char* gt = (const char*)memchr(after, '>', end - after);
    if (!gt) return 0;
    *out_attr_begin = after;
    *out_attr_end   = gt;
    *out_search_continue = gt + 1;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

/* Replace `<!-- ... -->` regions with spaces so the attribute scanner
 * doesn't pick up commented-out elements (e.g. L1 keeps a commented
 * CSnakeFactory which we used to parse as a live one). Operates in place. */
static void strip_xml_comments(char* buf, size_t len)
{
    char* p = buf;
    char* end = buf + len;
    while (p + 4 <= end) {
        if (p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            char* q = p + 4;
            while (q + 3 <= end &&
                   !(q[0] == '-' && q[1] == '-' && q[2] == '>')) {
                if (*q != '\n') *q = ' ';
                q++;
            }
            *p = ' '; *(p+1) = ' '; *(p+2) = ' '; *(p+3) = ' ';
            if (q + 3 <= end) { *q = ' '; *(q+1) = ' '; *(q+2) = ' '; }
            p = q + 3;
        } else {
            p++;
        }
    }
}

static int parse_buffer(char* buf, size_t len)
{
    flow_level_reset_defaults();
    strip_xml_comments(buf, len);

    char tmp[128];

    /* CLevel — root attributes */
    const char *abeg, *aend, *cont;
    if (find_element(buf, len, "CLevel", &abeg, &aend, &cont)) {
        if (attr_str(abeg, aend, "BotColor", tmp, sizeof(tmp)))
            parse_rgba(tmp, g_flow_level.bot_color);
        if (attr_str(abeg, aend, "MidColor", tmp, sizeof(tmp)))
            parse_rgba(tmp, g_flow_level.mid_color);
        if (attr_str(abeg, aend, "TopColor", tmp, sizeof(tmp)))
            parse_rgba(tmp, g_flow_level.top_color);
        if (attr_str(abeg, aend, "Radius", tmp, sizeof(tmp)))
            g_flow_level.radius = (float)atof(tmp);
        if (attr_str(abeg, aend, "Glow", tmp, sizeof(tmp)))
            g_flow_level.glow = (float)atof(tmp);
    } else {
        return 0;
    }

    /* Particles — sum all CParticleHerd Number, remember last non-zero
     * species name. */
    g_flow_level.particle_count = 0;
    const char* search = buf;
    size_t       remaining = len;
    while (search < buf + len &&
           find_element(search, remaining, "CParticleHerd",
                        &abeg, &aend, &cont))
    {
        int n = 0;
        if (attr_str(abeg, aend, "Number", tmp, sizeof(tmp)))
            n = atoi(tmp);
        g_flow_level.particle_count += n;
        if (n > 0 && attr_str(abeg, aend, "Species", tmp, sizeof(tmp))) {
            snprintf(g_flow_level.particle_species,
                     sizeof(g_flow_level.particle_species), "%s", tmp);
        }
        search    = cont;
        remaining = (buf + len) - search;
    }
    if (g_flow_level.particle_count == 0)
        g_flow_level.particle_count = 1800; /* fall back to default */

    /* Snake — first CSnakeFactory only (most levels have just one).
     * Default to 0 segments if no factory is present (some levels have a
     * commented-out CSnakeFactory, or none at all). */
    g_flow_level.snake_num_segs = 0;
    if (find_element(buf, len, "CSnakeFactory", &abeg, &aend, &cont)) {
        g_flow_level.snake_num_segs = 8;
        if (attr_str(abeg, aend, "NumSegs", tmp, sizeof(tmp)))
            g_flow_level.snake_num_segs = parse_range_int(tmp);
        if (attr_str(abeg, aend, "JointDist", tmp, sizeof(tmp)))
            g_flow_level.snake_joint_dist = parse_range_float(tmp);
        if (attr_str(abeg, aend, "Radius", tmp, sizeof(tmp)))
            g_flow_level.snake_radius = parse_range_float(tmp);
        if (attr_str(abeg, aend, "MaxSpeed", tmp, sizeof(tmp)))
            g_flow_level.snake_max_speed = parse_range_float(tmp);
    }

    /* Food factories — gather up to FLOW_LEVEL_MAX_FOODS. */
    g_flow_level.food_factory_count = 0;
    g_flow_level.total_food = 0;
    search    = buf;
    remaining = len;
    while (search < buf + len &&
           g_flow_level.food_factory_count < FLOW_LEVEL_MAX_FOODS &&
           find_element(search, remaining, "CFoodFactory",
                        &abeg, &aend, &cont))
    {
        FlowFoodFactory& f = g_flow_level.foods[g_flow_level.food_factory_count];
        f.num_objects = 0;
        f.num_nour    = 1;
        f.food_type[0] = '\0';
        if (attr_str(abeg, aend, "NumObjects", tmp, sizeof(tmp)))
            f.num_objects = parse_range_int(tmp);
        if (attr_str(abeg, aend, "FoodType", tmp, sizeof(tmp)))
            snprintf(f.food_type, sizeof(f.food_type), "%s", tmp);
        if (attr_str(abeg, aend, "NumNour", tmp, sizeof(tmp)))
            f.num_nour = parse_range_int(tmp);
        g_flow_level.total_food += f.num_objects;
        g_flow_level.food_factory_count++;
        search    = cont;
        remaining = (buf + len) - search;
    }

    g_flow_level.loaded = 1;
    return 1;
}

static int try_open_into(const char* path, char* out_path, size_t obytes,
                         char** out_buf, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1 * 1024 * 1024) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    snprintf(out_path, obytes, "%s", path);
    *out_buf = buf;
    *out_len = got;
    return 1;
}

int flow_level_load(const char* level_path)
{
    flow_level_reset_defaults();

    char  resolved[512];
    char* buf = NULL;
    size_t len = 0;
    int    ok  = 0;

    /* 1. Try as-is */
    if (!ok)
        ok = try_open_into(level_path, resolved, sizeof(resolved), &buf, &len);

    /* 2. Try under FLOW_GAME_DIR */
    if (!ok) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", FLOW_GAME_DIR, level_path);
        ok = try_open_into(path, resolved, sizeof(resolved), &buf, &len);
    }

    /* 3. Try under extracted/USRDIR (the canonical extracted dump) */
    if (!ok) {
        char path[512];
        snprintf(path, sizeof(path), "extracted/USRDIR/%s", level_path);
        ok = try_open_into(path, resolved, sizeof(resolved), &buf, &len);
    }

    /* 4. Try under <FLOW_GAME_DIR>/../extracted/USRDIR */
    if (!ok) {
        char path[512];
        snprintf(path, sizeof(path), "%s/../extracted/USRDIR/%s",
                 FLOW_GAME_DIR, level_path);
        ok = try_open_into(path, resolved, sizeof(resolved), &buf, &len);
    }

    if (!ok) {
        fprintf(stderr, "[level] Could not open %s — using defaults\n",
                level_path);
        return 0;
    }

    int parsed = parse_buffer(buf, len);
    free(buf);

    if (parsed) {
        snprintf(g_flow_level.source_path,
                 sizeof(g_flow_level.source_path), "%s", resolved);
        fprintf(stderr,
                "[level] Loaded %s\n"
                "[level]   gradient bot=(%.2f,%.2f,%.2f) mid=(%.2f,%.2f,%.2f) top=(%.2f,%.2f,%.2f) glow=%.2f\n"
                "[level]   particles=%d (%s) snake_segs=%d r=%.1f food_factories=%d total_food=%d\n",
                resolved,
                g_flow_level.bot_color[0], g_flow_level.bot_color[1], g_flow_level.bot_color[2],
                g_flow_level.mid_color[0], g_flow_level.mid_color[1], g_flow_level.mid_color[2],
                g_flow_level.top_color[0], g_flow_level.top_color[1], g_flow_level.top_color[2],
                g_flow_level.glow,
                g_flow_level.particle_count, g_flow_level.particle_species,
                g_flow_level.snake_num_segs, g_flow_level.snake_radius,
                g_flow_level.food_factory_count, g_flow_level.total_food);
        fflush(stderr);
    }
    return parsed;
}

int flow_level_load_campaign(int campaign, int level)
{
    char rel[256];
    snprintf(rel, sizeof(rel),
             "Data/Campaigns/Campaign_%d/v1/C%d_L%d_v1.xml",
             campaign, campaign, level);
    return flow_level_load(rel);
}
