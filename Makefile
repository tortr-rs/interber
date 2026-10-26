# interber
VERSION = 0.1
PREFIX = /usr/local

PKGS = gtk+-3.0 webkit2gtk-4.1
CFLAGS += -std=c99 -Wall -Os -D_DEFAULT_SOURCE -DVERSION=\"$(VERSION)\" `pkg-config --cflags $(PKGS)`
LDLIBS += `pkg-config --libs $(PKGS)`

interber: interber.c
	$(CC) $(CFLAGS) -o $@ interber.c $(LDFLAGS) $(LDLIBS)

install: interber
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)/usr/share/applications $(DESTDIR)/usr/share/fonts/interber
	install -m 755 interber $(DESTDIR)$(PREFIX)/bin/
	install -m 644 vga.ttf $(DESTDIR)/usr/share/fonts/interber/
	sed 's|@PREFIX@|$(PREFIX)|' interber.desktop > $(DESTDIR)/usr/share/applications/interber.desktop

uninstall:
	rm -rf $(DESTDIR)$(PREFIX)/bin/interber $(DESTDIR)/usr/share/fonts/interber $(DESTDIR)/usr/share/applications/interber.desktop

clean:
	rm -f interber

.PHONY: install uninstall clean
