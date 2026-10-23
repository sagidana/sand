CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
COMPDIR = $(DESTDIR)$(PREFIX)/share/bash-completion/completions

sand: sand.c
	$(CC) $(CFLAGS) -o $@ $<

install: sand
	install -D -m 755 sand $(BINDIR)/sand
	install -D -m 644 sand.bash $(COMPDIR)/sand

uninstall:
	$(RM) $(BINDIR)/sand $(COMPDIR)/sand

# Copy the example profiles to ~/.config/sand, keeping any that exist.
install-profiles:
	mkdir -p $${XDG_CONFIG_HOME:-$$HOME/.config}/sand
	cp -n profiles/* $${XDG_CONFIG_HOME:-$$HOME/.config}/sand/

test: sand
	./test.sh

clean:
	$(RM) sand

.PHONY: install install-profiles uninstall clean test
