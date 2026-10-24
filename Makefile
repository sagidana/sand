CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
COMPDIR = $(DESTDIR)$(PREFIX)/share/bash-completion/completions

all: sand

sand: sand.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -D -m 755 sand $(BINDIR)/sand
	install -D -m 644 sand.bash $(COMPDIR)/sand

# Ubuntu 23.10 and later: let sand create user namespaces.
install-apparmor:
	install -D -m 644 apparmor/sand /etc/apparmor.d/sand
	apparmor_parser -r /etc/apparmor.d/sand

uninstall:
	$(RM) $(BINDIR)/sand $(COMPDIR)/sand

test: all
	./test.sh

clean:
	$(RM) sand

.PHONY: all install uninstall clean test
