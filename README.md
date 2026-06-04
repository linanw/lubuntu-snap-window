# lubuntu-snap-window

Small X11 tools for Lubuntu/LXQt:
- `snapcorners`: snaps the active window when you finish dragging it into a screen corner.
- `focusnotify`: shows a desktop notification with width/height when window focus changes.

## What it does
- Drag a window and release in top-left: snaps to left 30% width and full usable height (screen height minus dock/panel area).
- Drag and release in top-right: snaps to right 70% width and full usable height.
- Drag and release on the top screen edge (not in corners): snaps to full usable work area size.
- Drag and release on the left or right screen edge: snaps to that side at 50% width and full usable height (screen height minus dock/panel area).
- Drag and release in bottom-left: snaps to the bottom-left quarter of the usable work area.
- Drag and release in bottom-right: snaps to the bottom-right quarter of the usable work area.
- When screen resolution changes, horizontal dock/panel windows are resized to fill full screen width.

Snap behaviour is fully customisable via a JSON profile file — see **[Snap Profiles](#snap-profiles)** below.

## Dependencies

- X11 development headers: `sudo apt-get install libx11-dev` (on Debian/Ubuntu)

## Build

```bash
make
```

## Install

Install to the default system location:

```bash
sudo make install
```

This installs the binaries into `/usr/local/bin`.

To install somewhere else:

```bash
make install PREFIX=/usr
make install BINDIR=$HOME/.local/bin
```

`DESTDIR` is for staging/package builds, not for choosing the final runtime path. Example:

```bash
make install DESTDIR=/tmp/pkg
```

This creates files under `/tmp/pkg/usr/local/bin`, not directly under `/usr/local/bin`.

## Run

```bash
./snapcorners
```

```bash
./focusnotify
```

Keep it running in the background:

```bash
nohup ./snapcorners >/tmp/snapcorners.log 2>&1 &
```

```bash
nohup ./focusnotify >/tmp/focusnotify.log 2>&1 &
```

## Autostart on Lubuntu (LXQt)

Quick way (recommended):

```bash
make autostart
```

Disable later:

```bash
make disable-autostart
```

Manual option: create `~/.config/autostart/snapcorners.desktop` with `Exec=/full/path/to/snapcorners`.

Then log out and log in again (or reboot).

## More reliable startup (systemd user service)

If `snapcorners` sometimes stops, run it as a user service so it starts automatically at login and restarts if it exits:

```bash
make systemd-user-enable
```

Check status:

```bash
systemctl --user status snapcorners.service
```

Check logs:

```bash
journalctl --user -u snapcorners.service -f
```

Disable service:

```bash
make systemd-user-disable
```

After changing the service settings, reload and restart it:

```bash
make systemd-user-restart
```

## Troubleshooting

1. Confirm you are in an X11 session (not Wayland):

```bash
echo "$XDG_SESSION_TYPE"
```

2. Run foreground with debug logs:

```bash
SNAPCORNERS_VERBOSE=1 ./snapcorners
```

3. If running by autostart, check process is alive:

```bash
pgrep -af snapcorners
```

4. If using `.desktop` autostart with `nohup`, inspect logs:

```bash
tail -n 200 /tmp/snapcorners.log
```

5. Rebuild after changes:

```bash
make clean && make
```

## Notes
- This tool is for X11 sessions.
- `snapcorners` uses the current screen size from X11 and snap rules from this repo.
- `focusnotify` uses `notify-send`; install `libnotify-bin` if notifications do not appear.

## Snap Profiles

Snap behaviour is configured through a JSON profile file. `snapcorners` loads it automatically at startup from:

```
$HOME/.config/snapcorners/profiles.json
```

Override the path with the `SNAPCORNERS_PROFILE` environment variable. If the file is absent, the built-in defaults are used.

A ready-to-use example is included in [profiles.json](profiles.json). Copy it to the config directory:

```bash
mkdir -p ~/.config/snapcorners
cp profiles.json ~/.config/snapcorners/profiles.json
```

### Profile structure

```json
{
  "profiles": [
    {
      "name": "default",
      "min_screen_width": 0,
      "max_screen_width": 1920,
      "triggers": { ... }
    },
    {
      "name": "wide",
      "min_screen_width": 1921,
      "max_screen_width": 99999,
      "triggers": { ... }
    }
  ]
}
```

`snapcorners` picks the **first** profile whose `[min_screen_width, max_screen_width]` range includes the current display width. The profile is re-evaluated whenever the screen resolution changes.

### Trigger keys

Each entry under `"triggers"` maps a drag-zone name to a **snap target** — where the window is placed relative to the usable work area:

| Key | Zone |
|---|---|
| `corner_top_left` | Mouse released in top-left pixel corner |
| `corner_top_right` | Mouse released in top-right pixel corner |
| `corner_bottom_left` | Mouse released in bottom-left pixel corner |
| `corner_bottom_right` | Mouse released in bottom-right pixel corner |
| `side_left` | Mouse released on left screen edge |
| `side_right` | Mouse released on right screen edge |
| `top_edge` | Mouse released on top screen edge (default, when no zone matches) |
| `top_edge_zones` | Array of sub-zones along the top edge (see below) |

### Snap target fields

```json
{ "x_frac": 0.0, "y_frac": 0.0, "w_frac": 0.5, "h_frac": 1.0 }
```

| Field | Meaning |
|---|---|
| `x_frac` | Left-edge offset from work area left, as a fraction of work area width |
| `y_frac` | Top-edge offset from work area top, as a fraction of work area height |
| `w_frac` | Window width as a fraction of work area width |
| `h_frac` | Window height as a fraction of work area height |

All values are in the range `[0.0, 1.0]`.

### Top-edge sub-zones

Use `top_edge_zones` to snap windows to different targets depending on **where along the top edge** the mouse is released. Each zone is matched by the mouse X position as a fraction of the full screen width:

```json
"top_edge_zones": [
  {
    "mouse_x_min_frac": 0.4444,
    "mouse_x_max_frac": 0.5556,
    "snap": { "x_frac": 0.3333, "y_frac": 0.0, "w_frac": 0.3333, "h_frac": 1.0 }
  }
]
```

The example above: when the mouse is dropped in the centre 1/9th of the top edge (positions 4/9 – 5/9 of screen width), the window snaps to a 1/3-width centred column. Zones are checked in order; if none match, `top_edge` is used.

### Built-in profiles example

The included [profiles.json](profiles.json) ships two profiles:

**`default`** (≤ 1920 px wide) — original behaviour:
- Top-left corner → left 30 %, full height
- Top-right corner → right 70 %, full height
- Left/right edge → left/right 50 %
- Top edge → maximise to work area

**`wide`** (> 1920 px wide) — optimised for ultrawide monitors:
- Top-left corner → left 25 %, full height
- Top-right corner → right 25 %, full height
- Left/right edge → left/right 50 %
- Top edge → maximise to work area
- Top edge centre zone (mouse in middle 1/9) → 1/3 width, centred

