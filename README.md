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
