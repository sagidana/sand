CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2
PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin

sandbox: sandbox.c
	$(CC) $(CFLAGS) -o $@ $<

install: sandbox
	install -D -m 755 sandbox $(BINDIR)/sandbox

uninstall:
	$(RM) $(BINDIR)/sandbox

clean:
	$(RM) sandbox

.PHONY: install uninstall clean
