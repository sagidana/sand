# sand

Run a command, or an interactive shell, in the current directory with
restrictions. One C file, Linux only, no root, no dependencies.

    sand                                   shell in the cwd, read-only, no network
    sand python script.py                  run a command instead of a shell
    sand --rw out make                     one writable directory
    sand --net host curl https://example.com
    sand --net host --allow :443 curl https://example.com
    sand --learn net pip install requests  allow everything, log what it reached
    sand -p dev make                       flags written once, in ~/.config/sand/dev

The sandbox is built from Linux namespaces: the command gets its own
user, mount, pid, ipc and uts namespaces, and by default its own empty
network namespace. Nothing is installed on the host and nothing runs as
root. Sandboxes nest.

## Build and install

    make
    make install                 # /usr/local/bin/sand
    make install PREFIX=~/.local # anywhere else

`make install` also puts a bash completion file under
`PREFIX/share/bash-completion/completions`, which the bash-completion
package picks up from `/usr/local` and from `~/.local`. It completes
flags, profile names and paths, and after them the command's own
completion takes over, as it does for `sudo`.

Requires a Linux kernel of 5.12 or newer (April 2021: Debian 12,
Ubuntu 22.04, RHEL 9 and later) with unprivileged user namespaces
enabled (the default on most distributions, and on WSL2), x86_64 or
aarch64, and a C compiler. It builds with older kernel headers too.

Ubuntu 23.10 and later let only programs with an AppArmor profile
create user namespaces (`kernel.apparmor_restrict_unprivileged_userns`),
and `sand` without one stops at "unshare: Operation not permitted"
with a line saying so. `apparmor/sand` is that profile, for `sand`
at `/usr/local/bin`; edit the path for another `PREFIX`, then

    sudo make install-apparmor

On the few systems with the older `kernel.unprivileged_userns_clone`
switched off, `sand` names that sysctl instead.

## What the command sees

An empty root containing only:

- the host's `/usr`, `/bin`, `/sbin`, `/lib`, `/lib64` and `/etc`, read-only,
  so a shell and the system's programs work
- the current working directory, read-only, at its own absolute path
- any paths given with `--ro` or `--rw`, at their own paths
- a fresh empty `/tmp`, a fresh `/proc`, and a minimal `/dev` with
  `null`, `zero`, `random`, `urandom`, `tty`, the stdin/stdout/stderr
  links, a private `/dev/shm` and a private set of pseudo-terminals

Nothing else from the host exists: no `/home`, no `/root`, no `/var`,
no `/opt`, no `/run`, no `/sys`. The environment is reduced to `PATH`,
`TERM`, `HOME`, `USER`, `LANG` and `SHELL`, plus `SAND=1`. No-new-privs
is set, so setuid programs such as `sudo` do nothing special.

The terminal belongs to the command while it runs, as with `sudo`:
Ctrl-C and Ctrl-Z go to it, not to `sand`, a shell inside has working
job control, and Ctrl-Z followed by `fg` or `bg` in the outer shell
behaves as for any other job. Started in the background with `&`, it
stays there: a command that reads from the terminal stops until `fg`.
Without a terminal, `SIGINT`, `SIGQUIT`, `SIGTERM` and `SIGHUP` sent to
`sand` are passed to the command. If `sand` itself is killed, the
sandbox and everything in it ends with it.

Inside, pid 1 is `sand`'s own init, which starts the command and reaps
for it. The command runs one user namespace below init, mapped to the
same uid, so it holds no capability over init, the mounts or the
network: it cannot ptrace init or read its memory, undo a mount, or
change an interface. It can still make namespaces of its own, which is
what nesting uses.

## Flags

    -p, --profile NAME   load the flags written in ~/.config/sand/NAME
    --ro PATH            also make PATH visible, read-only
    --rw PATH            also make PATH visible, writable
    --hide PATH          make a visible PATH appear empty
    --env NAME           pass the host's NAME through to the command
    --env NAME=VALUE     set NAME for the command
    --net none|host      loopback only (default), or the host's network
    --allow RULE         with --net host: allow matching traffic
    --block RULE         with --net host: block matching traffic
    --learn net          the host's network, nothing blocked, every destination and lookup logged
    COMMAND [ARG]...     run COMMAND instead of $SHELL

The first word that is not a flag starts the command. A command whose
name begins with `-` needs `--` in front of it.

Path flags take files or directories and may repeat. They apply in
order and a later flag wins, so `--rw .` makes the cwd writable and
`--ro ~ --hide ~/.ssh` shows the home directory without the keys.
`--net none` after network rules, or after `--learn net`, drops them.

## Profiles

A profile is a file of flags under `~/.config/sand` (or
`$XDG_CONFIG_HOME/sand`), one per line, the same words as on the
command line with or without the dashes. `#` starts a comment and `~`
at the start of a path means the home directory. `profile NAME` loads
another profile in place, which is how a hierarchy is built:

    # ~/.config/sand/net        dns and https, nothing else
    net host
    allow :53
    allow :443

    # ~/.config/sand/lan        the same, plus the local network
    profile net
    allow 10.0.0.0/8
    allow 192.168.0.0/16

    # ~/.config/sand/dev        a working setup for this machine
    profile lan
    rw .
    ro ~/.cache/pip
    env SSH_AUTH_SOCK

    sand -p dev make
    sand -p lan -p build ./run.sh
    sand -p dev --net none make test      flags after the profile override it

Profiles hold only flags, never a command. Everything is applied in
one sequence, later wins: the profile `default` first if it exists,
then the command line left to right, with each `-p` expanding in
place. Relative paths in a profile are relative to the directory
`sand` is started in, like on the command line. A `hide` of a path
that does not exist is a no-op, so one profile can serve machines
with different dotfiles.

The `profiles/` directory holds a few to start from; copy the ones
you want to `~/.config/sand`:

    net      host network, but only name lookup and https
    lan      like net, plus the private address ranges
    online   the host's network, unfiltered
    pypi     the names pip talks to, https only
    github   github.com and its content hosts, https and ssh
    rw       the current directory is writable
    home     the home directory read-only, minus keys and credentials

The command's exit status is returned. A command that cannot be found
gives 127, one that cannot be executed gives 126, like a shell.

## Network rules

`--net host` gives the command the host's network as is. Adding
`--allow` or `--block` rules filters it by address and port:

    RULE = [src:][ADDR[/PREFIX] | NAME][:PORT[-PORT]] | all

    1.1.1.1              one address, any port
    10.0.0.0/8:443       a range, one port
    :53                  any address, one port
    8000-9000            any address, a port range
    [::1]:80             IPv6 with a port needs brackets
    src:8080             a local port, applies to bind
    pypi.org:443         the addresses a name resolves to, one port
    *.github.com         a name and everything below it (quote the *)

Rules apply in order and a later rule wins. As soon as any `--allow`
is given, everything not allowed is blocked. Destination rules apply
to `connect` and to UDP sends that carry an address; `0.0.0.0` and `::`
as a destination mean this host and are matched as `127.0.0.1` and
`::1`, and IPv4 carried on an IPv6 socket as `::ffff:a.b.c.d` is
matched as the IPv4 address. `src:` rules apply to `bind`, which
covers listening sockets and explicit source ports, and to `listen` on
a socket that was never bound, which would otherwise take a port of
its own: that counts as a bind to port 0 on every address. A blocked
call fails with "Network is unreachable" (EACCES for bind and listen)
and a line in the log names the address and what decided it, the block
rule or the absence of a matching allow. A lookup of a name no rule
covers is logged once as well, so the rule to add is in the log:

    2026-10-25 09:12:41 sand[4123]: lookup of registry.npmjs.org matches no rule
    2026-10-25 09:12:41 sand[4123]: blocked connection to 104.16.1.35 port 443 (no allow rule or allowed name matches)

    sand --net host --allow :53 --allow :443 pip install requests
    sand --net host --block :25 ./app
    sand --net host --allow 10.0.0.0/8 --block 10.1.0.0/16 ./app

### The log

Everything the broker decides goes to `/tmp/sand.log`, or the file
named in `$SAND_LOG`, never to the terminal: blocked calls, lookups no
rule covers, and what learn mode sees. Every run appends to the same
file, each line stamped with the time and `sand`'s pid, so `tail -f
/tmp/sand.log` in another terminal follows a running command. The file
is created with mode 0600 and only when there is a filter to report
on; a plain `sand make` never touches it. If it cannot be opened, the
lines go to stderr with a note saying so.

### Learning

When wrapping a program whose network needs are not documented, start
with `--learn net`: the host's network, nothing blocked, and each
distinct destination, bind and name lookup logged once as it happens.
Run the thing to completion, then write the profile from the log:

    sand --learn net pip install requests
    tail /tmp/sand.log

    2026-10-25 09:14:02 sand[4201]: allowed connection to 127.0.0.53 port 53
    2026-10-25 09:14:02 sand[4201]: lookup of pypi.org
    2026-10-25 09:14:02 sand[4201]: allowed connection to 151.101.0.223 port 443
    2026-10-25 09:14:03 sand[4201]: lookup of files.pythonhosted.org
    2026-10-25 09:14:03 sand[4201]: allowed connection to 146.75.28.223 port 443

Lookups are read from the queries the command sends, with the same
limits as for name rules below; a program with its own resolver that
sends over a connected socket shows only addresses. `--allow` and
`--block` do not apply in learn mode and are refused after it;
`--learn net` after rules drops them, and `--net none` after it turns
it off. Learn mode uses the same filter as rules, so it cannot run
inside a filtered sandbox either. `net` is the only thing to learn so
far; the filesystem is decided once, at startup, with nothing to watch.

### Names

A rule written as a name stands for the addresses the name resolves
to, looked up by `sand` itself through the `/etc/hosts` and
`/etc/resolv.conf` the command sees. The first name rule also lets
port 53 through to the nameservers in `/etc/resolv.conf`, and to
nothing else, since a name rule without lookups is dead; a `block :53`
written after it still wins. `*.NAME` covers the name and anything
below it.

    sand --net host --allow pypi.org:443 --allow files.pythonhosted.org:443 pip install requests
    sand --net host --allow '*.github.com:443' --allow '*.githubusercontent.com:443' git pull

Addresses are learned two ways. When the command connects to an
address no rule settles, every name in the rules is resolved and the
call waits for it. And a lookup the broker sees names what to resolve
next time: that is how `*.github.com` comes to know `api.github.com`.
Learned addresses are kept for five minutes, then resolved again.

What a name rule is not: a check on the name a connection is for. The
broker judges addresses, and an address allowed for one name is open
to every site served on it, which on a shared CDN is many. `block
NAME` only covers the addresses the name resolves to; a program can
still connect to them by address. The broker sees a lookup only when
the query is sent with an address or as a batch (glibc's parallel A
and AAAA lookup, musl); a query written with `send(2)` on a connected
socket, as by Go's own resolver or a single-type lookup, is not seen,
and a `*.NAME` rule learns nothing from it, while a name written out
in a rule works either way. Lookups over TCP or HTTPS are not seen.
`--rw` on `/etc/hosts` hands the command the rule table.

How it works: the command runs under a seccomp filter that hands
`connect`, `bind`, `listen`, `sendmsg`, `sendmmsg` and address-carrying
`sendto` to the sandbox's init process. Init takes a handle on the
command's socket, copies the address out of the command's memory,
checks the rules and performs the call itself. Because the kernel
never re-reads the command's memory, a thread changing the address
after the check gains nothing. Plain `send`, `write` and all receives
are not trapped. A call that may block, such as `connect` on a
blocking socket, is performed by a short-lived child of init, so a hung
peer does not hold up the other sockets.

Init is out of the command's reach: the command runs in a user
namespace below it, where the same uid gives no right to ptrace init,
read its memory or take its descriptors (and init is non-dumpable
besides). The filter closes the ways around the trapped calls:

- `io_uring` is absent, since it can connect and send without a
  syscall the filter sees.
- `socket` opens only unix, IPv4 and IPv6 stream and datagram sockets
  with the common protocols (TCP, UDP, UDP-Lite, ICMP, MPTCP), routing
  netlink, and the kernel's crypto family. SCTP, which connects by
  `setsockopt`, and every family whose addresses the broker does not
  read, are refused.
- A unix socket may use any path the mount namespace shows, but not an
  abstract name: those live in the network namespace, where with
  `--net host` every service of the host listens. Connecting to one is
  refused and binding one is denied.
- Syscalls of another ABI (32-bit, x32) kill the process rather than
  being misread.

Side effects of the broker performing the call: a unix peer that reads
`SO_PEERCRED` sees init's pid, not the command's, and a socket
received over a unix socket from a host service passed in with `--rw`
is a connection the rules never saw.

The kernel allows one such filter per process chain, so a filtered
sandbox (rules or learn mode) cannot run inside another filtered sandbox. The inner one
refuses with a message; an unfiltered inner sandbox still runs under
the outer rules.

## How it works

Three processes, one of them on the host:

    your shell ──runs──▶ sand ──unshare, fork──▶ init (pid 1) ──fork, exec──▶ command
                          ▲                        │  ▲                          │
                          └── stop reported ───────┘  └── trapped socket calls ──┘

`sand` parses the flags and profiles, unshares the user, mount, pid,
ipc and uts namespaces (net too, unless `--net host`) and forks
`init`, which is pid 1 of the new pid namespace and dies with `sand`
(`PR_SET_PDEATHSIG`), taking the sandbox with it. Init stages the root
on a tmpfs: the system directories read-only, a fresh `/proc`, `/tmp`
and a minimal `/dev`, the cwd and every `--ro` and `--rw` at their own
paths in flag order, `--hide` covered with something empty, then
`pivot_root` and a read-only remount of `/`. Init then forks the
command one user namespace lower, mapped to the same uid, so the
command holds no capability over init, the mounts or the network,
while init keeps every capability over the command. Init reaps,
forwards signals, and reports a stop over a pipe so the host shell's
job stops like any other.

With network rules the command runs under a seccomp filter whose
address-carrying calls are answered by init, the broker: it takes a
copy of the socket, copies the address out of the command's memory,
walks the rules and performs the call itself. Anything that could
block init, or needs a name resolved, is done again in a short-lived
worker forked from init, which reports what it learned over a pipe
before it answers. The details are under Network rules above.

Rules, paths and settings form one flat sequence, later wins: the
profile `default`, then the command line with each `-p` expanded in
place. Paths become binds in that order, `net` is a single setting,
and the last matching `allow` or `block` decides an address.

## Nesting

`sand` inside `sand` works, as long as the binary is visible
inside (it is if it is installed under `/usr`, or pass `--ro` for it).
Each level can only restrict further: `--net host` inside a `--net
none` sandbox is that empty network. The inner `sand` only sees
profiles if `~/.config/sand` is passed in with `--ro`.

## Tests

    make test                    # or ./test.sh [NAME...]

`test.sh` runs the built binary on small cases and prints one line per
case. The listener and terminal cases need `python3`.

## Not covered

Things this tool does not do today. Some are planned, some are out of
scope by design.

Resource limits. Nothing stops a fork bomb, a memory hog or a runaway
CPU loop, and nothing limits how much a `--rw` directory or `/tmp` can
grow. There is no wall-clock timeout either; use `timeout(1)` in front.
Real limits need cgroups, which an unprivileged process only gets when
the system delegates a cgroup to it.

Rules on the name a connection is for. A name in a rule is resolved
to addresses; nothing reads the Host header or the TLS server name, so
an address shared between sites is shared in the rules too.

Root inside. The command runs as your own user, not as root in a user
namespace. `apt install`, `chown` to another user, `mknod`, mounting
filesystems and any setuid program do not work. Files owned by other
users appear as owned by nobody and keep their permissions.

Devices by default. USB, GPUs, audio, cameras and serial ports are
absent, and there is no `/sys`. A device node passed with `--rw` works
as it does outside, with the host's permissions on it: `--rw
/dev/bus/usb` lets `adb` see the phone, though a program that
enumerates through `/sys/bus/usb` needs `--ro /sys/bus/usb --ro
/sys/devices` too. Often the better way is to leave the device with a
service on the host and allow only its port, as with
`--net host --allow 127.0.0.1:5037 adb devices`.

Talking to host services. Sockets for X11, Wayland, D-Bus, ssh-agent,
gpg-agent and Docker live under `/tmp`, `/run` or the home directory
and are hidden. Each can be let in with `--rw` on the socket path plus
`--env` for the variable that names it, but nothing does it for you.

Software outside the system directories. Toolchains under the home
directory, `/opt`, `/snap` or `/nix` have to be passed with `--ro`.
`PATH` is inherited, so a missing directory silently falls through to
the system version of a program.

Throw-away writes. There is no copy-on-write mode where the command
may write anywhere and the changes are discarded afterwards. Overlay
mounts are available to user namespaces and would provide it.

Information in the system directories. `/etc` and `/proc` reveal
host names, user lists, machine ids, CPU and memory details. `--hide`
can cover individual paths; nothing hides them by default.

A security boundary against the kernel. The sandbox is a policy tool
built on unprivileged user namespaces, which are themselves a kernel
attack surface. It keeps honest programs from touching what they
should not and makes mistakes harmless. It is not a defense against a
kernel exploit, and apart from the network filter there is no seccomp
syscall allowlist.

Linux only. Namespaces are a Linux feature.
WSL2 works, WSL1 does not.
