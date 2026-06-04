CC = /usr/bin/gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lX11
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGETS = snapcorners focusnotify
AUTOSTART_DIR = $(HOME)/.config/autostart
AUTOSTART_FILE = $(AUTOSTART_DIR)/snapcorners.desktop
SYSTEMD_USER_DIR = $(HOME)/.config/systemd/user
SYSTEMD_USER_UNIT = $(SYSTEMD_USER_DIR)/snapcorners.service
SNAPCORNERS_SYSTEMD_BIN ?= /usr/local/bin/snapcorners

.PHONY: all clean install autostart disable-autostart systemd-user-install systemd-user-enable systemd-user-disable systemd-user-restart

all: $(TARGETS)

snapcorners: snapcorners.c snapconfig.c snapconfig.h
	$(CC) $(CFLAGS) -o $@ snapcorners.c snapconfig.c $(LDFLAGS) $(LDLIBS)

focusnotify: focusnotify.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: $(TARGETS)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGETS) "$(DESTDIR)$(BINDIR)"

clean:
	rm -f $(TARGETS)

autostart: snapcorners
	install -d "$(AUTOSTART_DIR)"
	printf '%s\n' \
		'[Desktop Entry]' \
		'Type=Application' \
		'Name=Snap Corners' \
		'Exec=$(CURDIR)/snapcorners' \
		'X-GNOME-Autostart-enabled=true' \
		'Terminal=false' > "$(AUTOSTART_FILE)"
	@echo "Autostart enabled: $(AUTOSTART_FILE)"

disable-autostart:
	rm -f "$(AUTOSTART_FILE)"
	@echo "Autostart disabled: $(AUTOSTART_FILE)"

systemd-user-install: snapcorners
	install -d "$(SYSTEMD_USER_DIR)"
	printf '%s\n' \
		'[Unit]' \
		'Description=Snap Corners (X11 window snap helper)' \
		'After=graphical-session.target' \
		'PartOf=graphical-session.target' \
		'StartLimitIntervalSec=0' \
		'' \
		'[Service]' \
		'Type=simple' \
		'ExecStart=$(SNAPCORNERS_SYSTEMD_BIN)' \
		'Restart=always' \
		'RestartSec=1' \
		'Environment=SNAPCORNERS_VERBOSE=1' \
		'' \
		'[Install]' \
		'WantedBy=default.target' > "$(SYSTEMD_USER_UNIT)"
	@echo "systemd user unit installed: $(SYSTEMD_USER_UNIT)"
	@echo "Then run: make systemd-user-enable"

systemd-user-enable: systemd-user-install
	systemctl --user daemon-reload
	systemctl --user enable --now snapcorners.service
	@echo "systemd user service enabled and started: snapcorners.service"

systemd-user-disable:
	systemctl --user disable --now snapcorners.service || true
	@echo "systemd user service disabled: snapcorners.service"

systemd-user-restart: systemd-user-install
	systemctl --user daemon-reload
	systemctl --user restart snapcorners.service
	@echo "systemd user service restarted: snapcorners.service"
