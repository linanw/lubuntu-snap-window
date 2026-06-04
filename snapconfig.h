#ifndef SNAPCONFIG_H
#define SNAPCONFIG_H

#define SNAP_MAX_PROFILES    16
#define SNAP_MAX_TOP_ZONES   16
#define SNAP_MAX_SIDE_ZONES  16

/*
 * A snap target describes where a window is placed relative to the usable
 * work area.  All fields are fractions in [0, 1]:
 *   x_frac  – left edge offset as fraction of workarea width
 *   y_frac  – top  edge offset as fraction of workarea height
 *   w_frac  – window width    as fraction of workarea width
 *   h_frac  – window height   as fraction of workarea height
 */
typedef struct {
    double x_frac;
    double y_frac;
    double w_frac;
    double h_frac;
} SnapTarget;

/*
 * A top-edge zone: when the mouse x position (as a fraction of screen width)
 * falls in [mouse_x_min_frac, mouse_x_max_frac], use this snap target instead
 * of the default top_edge target.
 */
typedef struct {
    double     mouse_x_min_frac;
    double     mouse_x_max_frac;
    SnapTarget target;
} TopEdgeZone;

/*
 * A side-edge zone: when the mouse y position (as a fraction of screen
 * height) falls in [mouse_y_min_frac, mouse_y_max_frac], use this snap target
 * instead of the default left/right side target.
 */
typedef struct {
    double     mouse_y_min_frac;
    double     mouse_y_max_frac;
    SnapTarget target;
} SideEdgeZone;

/*
 * The full set of snap actions for one profile.
 * corner[0]=top-left  corner[1]=top-right
 * corner[2]=bot-left  corner[3]=bot-right
 * side[0]=left        side[1]=right
 */
typedef struct {
    SnapTarget  corner[4];
    SnapTarget  side[2];
    SnapTarget  top_edge;
    SnapTarget  bottom_edge;
    TopEdgeZone top_zones[SNAP_MAX_TOP_ZONES];
    int         n_top_zones;
    TopEdgeZone bottom_zones[SNAP_MAX_TOP_ZONES];
    int         n_bottom_zones;
    SideEdgeZone left_zones[SNAP_MAX_SIDE_ZONES];
    int          n_left_zones;
    SideEdgeZone right_zones[SNAP_MAX_SIDE_ZONES];
    int          n_right_zones;
    int         valid;   /* non-zero once loaded */
} SnapTriggers;

typedef struct {
    char         name[64];
    int          min_screen_width;
    int          max_screen_width;
    SnapTriggers triggers;
} SnapProfile;

typedef struct {
    SnapProfile profiles[SNAP_MAX_PROFILES];
    int         n_profiles;
} SnapConfig;

/*
 * Load a JSON profile file into *cfg.
 * Returns 1 on success, 0 on error (file missing / parse error).
 */
int load_snap_config(const char *path, SnapConfig *cfg);

/*
 * Return the first profile whose [min,max] screen-width range includes
 * screen_width, or NULL if none match.
 */
const SnapProfile *select_profile(const SnapConfig *cfg, int screen_width);

/*
 * Return the path to the config file:
 *   - $SNAPCORNERS_PROFILE if set, otherwise
 *   - $HOME/.config/snapcorners/profiles.json
 * Returns NULL if HOME is unset and SNAPCORNERS_PROFILE is unset.
 */
const char *get_config_path(void);

#endif /* SNAPCONFIG_H */
