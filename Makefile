CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

sand: sand.c
	$(CC) $(CFLAGS) -o $@ $<

install: sand
	install -D -m 755 sand $(BINDIR)/sand

uninstall:
	$(RM) $(BINDIR)/sand

clean:
	$(RM) sand

.PHONY: install uninstall clean
