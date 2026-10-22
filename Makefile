CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2

sandbox: sandbox.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(RM) sandbox

.PHONY: clean
