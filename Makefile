CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
COMPDIR = $(DESTDIR)$(PREFIX)/share/bash-completion/completions

all: sand sand-net

sand: sand.c
	$(CC) $(CFLAGS) -o $@ $<

sand-net: sand-net.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -D -m 755 sand $(BINDIR)/sand
	install -D -m 755 sand-net $(BINDIR)/sand-net
	install -D -m 644 sand.bash $(COMPDIR)/sand

uninstall:
	$(RM) $(BINDIR)/sand $(BINDIR)/sand-net $(COMPDIR)/sand

test: all
	./test.sh

clean:
	$(RM) sand sand-net

.PHONY: all install uninstall clean test
