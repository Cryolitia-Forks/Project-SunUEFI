# SPDX-License-Identifier: MIT
OUT ?= ../../../build/camera
CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?=
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBEXECDIR ?= $(PREFIX)/libexec
LICENSEDIR ?= $(PREFIX)/share/licenses/piano-camera

OBJECTS = $(OUT)/piano-camerad.o $(OUT)/piano-camera-ctl.o

.PHONY: all install clean
all: $(OUT)/piano-camerad $(OUT)/piano-camera-ctl

$(OUT)/%.o: %.c
	mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c17 -MMD -MP -c "$<" -o "$@"

$(OUT)/piano-camerad: $(OUT)/piano-camerad.o
	$(CC) $(LDFLAGS) $^ -lm -o "$@"

$(OUT)/piano-camera-ctl: $(OUT)/piano-camera-ctl.o
	$(CC) $(LDFLAGS) $^ -o "$@"

install: all
	install -Dm755 "$(OUT)/piano-camerad" "$(DESTDIR)$(LIBEXECDIR)/piano-camerad"
	install -Dm755 "$(OUT)/piano-camera-ctl" "$(DESTDIR)$(BINDIR)/piano-camera-ctl"
	install -Dm644 LICENSE-MIT "$(DESTDIR)$(LICENSEDIR)/LICENSE-MIT"

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) $(OUT)/piano-camerad $(OUT)/piano-camera-ctl

-include $(OBJECTS:.o=.d)
