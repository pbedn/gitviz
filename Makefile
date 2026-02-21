CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
INCLUDES := -Iinclude
LDFLAGS := -Llib
LDLIBS := -lraylib -lm -ldl -lpthread -lGL -lrt -lX11

TARGET := build/gitviz
TEST_TARGET := build/test_gitviz
SRC := gitviz.c
TEST_SRC := tests/test_gitviz.c
REPO ?= $(CURDIR)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESKTOPDIR ?= $(DATADIR)/applications
ICONDIR ?= $(DATADIR)/pixmaps
ICON_SRC ?= docs/res/gitviz-0.1.jpg
APP_ID ?= gitviz
APP_SHAREDIR ?= $(DATADIR)/gitviz
FONTDIR ?= $(APP_SHAREDIR)/fonts
FONTS := assets/fonts/UbuntuMono-R.ttf assets/fonts/Ubuntu-R.ttf

.PHONY: all run test install uninstall install-desktop uninstall-desktop clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): $(TEST_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

run: $(TARGET)
	./$(TARGET) "$(REPO)"

install: $(TARGET) install-desktop
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 "$(TARGET)" "$(DESTDIR)$(BINDIR)/gitviz"

uninstall: uninstall-desktop
	rm -f "$(DESTDIR)$(BINDIR)/gitviz"

install-desktop:
	install -d "$(DESTDIR)$(FONTDIR)"
	install -m 644 $(FONTS) "$(DESTDIR)$(FONTDIR)"
	install -d "$(DESTDIR)$(ICONDIR)"
	install -m 644 "$(ICON_SRC)" "$(DESTDIR)$(ICONDIR)/$(APP_ID).jpg"
	install -d "$(DESTDIR)$(DESKTOPDIR)"
	printf '%s\n' \
		'[Desktop Entry]' \
		'Type=Application' \
		'Name=gitviz' \
		'Comment=Local Git visualizer' \
		'Exec=$(BINDIR)/gitviz' \
		'Icon=$(ICONDIR)/$(APP_ID).jpg' \
		'Terminal=false' \
		'Categories=Development;Utility;' \
	> "$(DESTDIR)$(DESKTOPDIR)/$(APP_ID).desktop"

uninstall-desktop:
	rm -f "$(DESTDIR)$(DESKTOPDIR)/$(APP_ID).desktop"
	rm -f "$(DESTDIR)$(ICONDIR)/$(APP_ID).jpg"
	rm -rf "$(DESTDIR)$(APP_SHAREDIR)"

clean:
	rm -f $(TARGET) $(TEST_TARGET)
