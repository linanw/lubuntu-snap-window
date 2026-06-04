#define _POSIX_C_SOURCE 200809L

#include "snapconfig.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Built-in defaults (mirror the original hard-coded behaviour) ────────── */

static const SnapTarget DEFAULT_CORNERS[4] = {
    {0.00, 0.0, 0.30, 1.0}, /* top-left:     30 % left,  full height */
    {0.30, 0.0, 0.70, 1.0}, /* top-right:    70 % right, full height */
    {0.00, 0.5, 0.50, 0.5}, /* bottom-left:  left  quarter           */
    {0.50, 0.5, 0.50, 0.5}, /* bottom-right: right quarter           */
};

static const SnapTarget DEFAULT_SIDES[2] = {
    {0.00, 0.0, 0.50, 1.0}, /* left  half */
    {0.50, 0.0, 0.50, 1.0}, /* right half */
};

static const SnapTarget DEFAULT_TOP = {0.00, 0.0, 1.00, 1.0};

static const SnapTarget DEFAULT_LEFT = {0.00, 0.0, 0.50, 1.0};
static const SnapTarget DEFAULT_RIGHT = {0.50, 0.0, 0.50, 1.0};

/* ── Minimal recursive-descent JSON parser ──────────────────────────────── */

typedef struct {
    const char *src;
    size_t      pos;
    size_t      len;
} Parser;

static void skip_ws(Parser *p)
{
    while (p->pos < p->len && isspace((unsigned char)p->src[p->pos]))
        p->pos++;
}

static int peek_char(Parser *p)
{
    skip_ws(p);
    if (p->pos >= p->len) return -1;
    return (unsigned char)p->src[p->pos];
}

static int consume_char(Parser *p, char c)
{
    skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == c) {
        p->pos++;
        return 1;
    }
    return 0;
}

/* Parse a JSON string into buf (basic; no escape processing needed here). */
static int parse_string(Parser *p, char *buf, size_t bufsz)
{
    skip_ws(p);
    if (p->pos >= p->len || p->src[p->pos] != '"') return 0;
    p->pos++; /* opening " */
    size_t start = p->pos;
    while (p->pos < p->len && p->src[p->pos] != '"')
        p->pos++;
    if (p->pos >= p->len) return 0;
    size_t slen = p->pos - start;
    if (slen >= bufsz) slen = bufsz - 1;
    memcpy(buf, p->src + start, slen);
    buf[slen] = '\0';
    p->pos++; /* closing " */
    return 1;
}

/* Parse a JSON number as double. */
static int parse_double(Parser *p, double *out)
{
    skip_ws(p);
    if (p->pos >= p->len) return 0;
    char *end;
    errno = 0;
    *out = strtod(p->src + p->pos, &end);
    if (end == p->src + p->pos || errno == ERANGE) return 0;
    p->pos = (size_t)(end - p->src);
    return 1;
}

static int parse_int(Parser *p, int *out)
{
    double d;
    if (!parse_double(p, &d)) return 0;
    *out = (int)d;
    return 1;
}

/* Forward declaration for recursive skip. */
static int skip_value(Parser *p);

static int skip_value(Parser *p)
{
    int c = peek_char(p);
    if (c < 0) return 0;

    if (c == '"') {
        char buf[256];
        return parse_string(p, buf, sizeof(buf));
    }
    if (c == '{') {
        p->pos++;
        if (peek_char(p) == '}') { p->pos++; return 1; }
        do {
            char key[64];
            if (!parse_string(p, key, sizeof(key))) return 0;
            if (!consume_char(p, ':'))              return 0;
            if (!skip_value(p))                     return 0;
        } while (consume_char(p, ','));
        return consume_char(p, '}');
    }
    if (c == '[') {
        p->pos++;
        if (peek_char(p) == ']') { p->pos++; return 1; }
        do {
            if (!skip_value(p)) return 0;
        } while (consume_char(p, ','));
        return consume_char(p, ']');
    }
    /* number / true / false / null */
    while (p->pos < p->len) {
        char ch = p->src[p->pos];
        if (ch == ',' || ch == '}' || ch == ']' || isspace((unsigned char)ch))
            break;
        p->pos++;
    }
    return 1;
}

/* Parse {"x_frac":…, "y_frac":…, "w_frac":…, "h_frac":…} */
static int parse_snap_target(Parser *p, SnapTarget *t)
{
    if (!consume_char(p, '{')) return 0;
    if (peek_char(p) == '}') { p->pos++; return 1; }
    do {
        char key[64];
        if (!parse_string(p, key, sizeof(key))) return 0;
        if (!consume_char(p, ':'))              return 0;
        double val;
        if (!parse_double(p, &val)) return 0;
        if      (strcmp(key, "x_frac") == 0) t->x_frac = val;
        else if (strcmp(key, "y_frac") == 0) t->y_frac = val;
        else if (strcmp(key, "w_frac") == 0) t->w_frac = val;
        else if (strcmp(key, "h_frac") == 0) t->h_frac = val;
        /* unknown keys inside a snap target are silently ignored */
    } while (consume_char(p, ','));
    return consume_char(p, '}');
}

/* Parse one element of top_edge_zones array. */
static int parse_top_zone(Parser *p, TopEdgeZone *z)
{
    if (!consume_char(p, '{')) return 0;
    if (peek_char(p) == '}') { p->pos++; return 1; }
    do {
        char key[64];
        if (!parse_string(p, key, sizeof(key))) return 0;
        if (!consume_char(p, ':'))              return 0;
        if (strcmp(key, "mouse_x_min_frac") == 0) {
            if (!parse_double(p, &z->mouse_x_min_frac)) return 0;
        } else if (strcmp(key, "mouse_x_max_frac") == 0) {
            if (!parse_double(p, &z->mouse_x_max_frac)) return 0;
        } else if (strcmp(key, "snap") == 0) {
            if (!parse_snap_target(p, &z->target)) return 0;
        } else {
            if (!skip_value(p)) return 0;
        }
    } while (consume_char(p, ','));
    return consume_char(p, '}');
}

static int parse_side_zone(Parser *p, SideEdgeZone *z)
{
    if (!consume_char(p, '{')) return 0;
    if (peek_char(p) == '}') { p->pos++; return 1; }
    do {
        char key[64];
        if (!parse_string(p, key, sizeof(key))) return 0;
        if (!consume_char(p, ':'))              return 0;
        if (strcmp(key, "mouse_y_min_frac") == 0) {
            if (!parse_double(p, &z->mouse_y_min_frac)) return 0;
        } else if (strcmp(key, "mouse_y_max_frac") == 0) {
            if (!parse_double(p, &z->mouse_y_max_frac)) return 0;
        } else if (strcmp(key, "snap") == 0) {
            if (!parse_snap_target(p, &z->target)) return 0;
        } else {
            if (!skip_value(p)) return 0;
        }
    } while (consume_char(p, ','));
    return consume_char(p, '}');
}

/* Parse the "triggers" object, filling in defaults for any missing key. */
static int parse_triggers(Parser *p, SnapTriggers *t)
{
    /* Start with built-in defaults. */
    memcpy(t->corner,    DEFAULT_CORNERS, sizeof(t->corner));
    memcpy(t->side,      DEFAULT_SIDES,   sizeof(t->side));
    t->top_edge    = DEFAULT_TOP;
    t->bottom_edge = DEFAULT_TOP;
    t->n_top_zones = 0;
    t->n_bottom_zones = 0;
    t->n_left_zones = 0;
    t->n_right_zones = 0;
    t->valid       = 1;

    if (!consume_char(p, '{')) return 0;
    if (peek_char(p) == '}') { p->pos++; return 1; }
    do {
        char key[64];
        if (!parse_string(p, key, sizeof(key))) return 0;
        if (!consume_char(p, ':'))              return 0;

        if      (strcmp(key, "corner_top_left")     == 0) { if (!parse_snap_target(p, &t->corner[0])) return 0; }
        else if (strcmp(key, "corner_top_right")    == 0) { if (!parse_snap_target(p, &t->corner[1])) return 0; }
        else if (strcmp(key, "corner_bottom_left")  == 0) { if (!parse_snap_target(p, &t->corner[2])) return 0; }
        else if (strcmp(key, "corner_bottom_right") == 0) { if (!parse_snap_target(p, &t->corner[3])) return 0; }
        else if (strcmp(key, "side_left")           == 0) { if (!parse_snap_target(p, &t->side[0]))   return 0; }
        else if (strcmp(key, "side_right")          == 0) { if (!parse_snap_target(p, &t->side[1]))   return 0; }
        else if (strcmp(key, "top_edge")            == 0) { if (!parse_snap_target(p, &t->top_edge))  return 0; }
        else if (strcmp(key, "bottom_edge")         == 0) { if (!parse_snap_target(p, &t->bottom_edge)) return 0; }
        else if (strcmp(key, "top_edge_zones") == 0 || strcmp(key, "bottom_edge_zones") == 0) {
            TopEdgeZone *zones = (strcmp(key, "top_edge_zones") == 0) ? t->top_zones : t->bottom_zones;
            int *zone_count = (strcmp(key, "top_edge_zones") == 0) ? &t->n_top_zones : &t->n_bottom_zones;
            if (!consume_char(p, '[')) return 0;
            if (peek_char(p) != ']') {
                do {
                    if (*zone_count >= SNAP_MAX_TOP_ZONES) {
                        if (!skip_value(p)) return 0;
                        continue;
                    }
                    TopEdgeZone *z = &zones[*zone_count];
                    z->mouse_x_min_frac = 0.0;
                    z->mouse_x_max_frac = 1.0;
                    z->target           = DEFAULT_TOP;
                    if (!parse_top_zone(p, z)) return 0;
                    (*zone_count)++;
                } while (consume_char(p, ','));
            }
            if (!consume_char(p, ']')) return 0;
        } else if (strcmp(key, "left_edge_zones") == 0 || strcmp(key, "right_edge_zones") == 0) {
            SideEdgeZone *zones = (strcmp(key, "left_edge_zones") == 0) ? t->left_zones : t->right_zones;
            int *zone_count = (strcmp(key, "left_edge_zones") == 0) ? &t->n_left_zones : &t->n_right_zones;
            const SnapTarget *default_target = (strcmp(key, "left_edge_zones") == 0) ? &DEFAULT_LEFT : &DEFAULT_RIGHT;
            if (!consume_char(p, '[')) return 0;
            if (peek_char(p) != ']') {
                do {
                    if (*zone_count >= SNAP_MAX_SIDE_ZONES) {
                        if (!skip_value(p)) return 0;
                        continue;
                    }
                    SideEdgeZone *z = &zones[*zone_count];
                    z->mouse_y_min_frac = 0.0;
                    z->mouse_y_max_frac = 1.0;
                    z->target = *default_target;
                    if (!parse_side_zone(p, z)) return 0;
                    (*zone_count)++;
                } while (consume_char(p, ','));
            }
            if (!consume_char(p, ']')) return 0;
        } else {
            if (!skip_value(p)) return 0;
        }
    } while (consume_char(p, ','));
    return consume_char(p, '}');
}

/* Parse a single profile object. */
static int parse_profile(Parser *p, SnapProfile *prof)
{
    memset(prof, 0, sizeof(*prof));
    prof->min_screen_width = 0;
    prof->max_screen_width = 99999;
    /* pre-fill triggers with defaults so partial configs work */
    memcpy(prof->triggers.corner, DEFAULT_CORNERS, sizeof(prof->triggers.corner));
    memcpy(prof->triggers.side,   DEFAULT_SIDES,   sizeof(prof->triggers.side));
    prof->triggers.top_edge = DEFAULT_TOP;
    prof->triggers.bottom_edge = DEFAULT_TOP;
    prof->triggers.valid    = 1;

    if (!consume_char(p, '{')) return 0;
    if (peek_char(p) == '}') { p->pos++; return 1; }
    do {
        char key[64];
        if (!parse_string(p, key, sizeof(key))) return 0;
        if (!consume_char(p, ':'))              return 0;

        if      (strcmp(key, "name")             == 0) { if (!parse_string(p, prof->name, sizeof(prof->name))) return 0; }
        else if (strcmp(key, "min_screen_width") == 0) { if (!parse_int(p, &prof->min_screen_width))           return 0; }
        else if (strcmp(key, "max_screen_width") == 0) { if (!parse_int(p, &prof->max_screen_width))           return 0; }
        else if (strcmp(key, "triggers")         == 0) { if (!parse_triggers(p, &prof->triggers))              return 0; }
        else { if (!skip_value(p)) return 0; }
    } while (consume_char(p, ','));
    return consume_char(p, '}');
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int load_snap_config(const char *path, SnapConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);

    if (fsz <= 0 || fsz > 1024L * 1024L) { fclose(f); return 0; }

    char *buf = malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return 0; }

    size_t n = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    buf[n] = '\0';

    Parser p = { buf, 0, n };
    int ok = 0;

    if (!consume_char(&p, '{')) goto done;
    if (peek_char(&p) != '}') {
        do {
            char key[64];
            if (!parse_string(&p, key, sizeof(key))) goto done;
            if (!consume_char(&p, ':'))              goto done;
            if (strcmp(key, "profiles") == 0) {
                if (!consume_char(&p, '[')) goto done;
                if (peek_char(&p) != ']') {
                    do {
                        if (cfg->n_profiles >= SNAP_MAX_PROFILES) {
                            skip_value(&p);
                            continue;
                        }
                        if (!parse_profile(&p, &cfg->profiles[cfg->n_profiles]))
                            goto done;
                        cfg->n_profiles++;
                    } while (consume_char(&p, ','));
                }
                if (!consume_char(&p, ']')) goto done;
            } else {
                if (!skip_value(&p)) goto done;
            }
        } while (consume_char(&p, ','));
    }
    consume_char(&p, '}');
    ok = (cfg->n_profiles > 0);

done:
    free(buf);
    return ok;
}

const SnapProfile *select_profile(const SnapConfig *cfg, int screen_width)
{
    for (int i = 0; i < cfg->n_profiles; i++) {
        const SnapProfile *pr = &cfg->profiles[i];
        if (screen_width >= pr->min_screen_width &&
            screen_width <= pr->max_screen_width)
            return pr;
    }
    return NULL;
}

const char *get_config_path(void)
{
    static char path[4096];
    const char *env = getenv("SNAPCORNERS_PROFILE");
    if (env && *env) return env;
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(path, sizeof(path), "%s/.config/snapcorners/profiles.json", home);
    return path;
}
