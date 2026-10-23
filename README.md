# sand

Run a command, or an interactive shell, in the current directory with
restrictions. One C file, Linux only, no root, no dependencies.

    sand                                   shell in the cwd, read-only, no network
    sand python script.py                  run a command instead of a shell
    sand --rw out make                     one writable directory
    sand --net host --allow :443 curl https://example.com
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
    COMMAND [ARG]...     run COMMAND instead of $SHELL

The first word that is not a flag starts the command. A command whose
name begins with `-` needs `--` in front of it.

Path flags take files or directories and may repeat. They apply in
order and a later flag wins, so `--rw .` makes the cwd writable and
`--ro ~ --hide ~/.ssh` shows the home directory without the keys.
`--net none` after network rules drops them.

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
    rw       the current directory is writable
    home     the home directory read-only, minus keys and credentials

The command's exit status is returned. A command that cannot be found
gives 127, one that cannot be executed gives 126, like a shell.

## Network rules

`--net host` gives the command the host's network as is. Adding
`--allow` or `--block` rules filters it by address and port:

    RULE = [src:][ADDR[/PREFIX]][:PORT[-PORT]] | all

    1.1.1.1              one address, any port
    10.0.0.0/8:443       a range, one port
    :53                  any address, one port
    8000-9000            any address, a port range
    [::1]:80             IPv6 with a port needs brackets
    src:8080             a local port, applies to bind

Rules apply in order and a later rule wins. As soon as any `--allow`
is given, everything not allowed is blocked. Destination rules apply
to `connect` and to UDP sends that carry an address; `0.0.0.0` and `::`
as a destination mean this host and are matched as `127.0.0.1` and
`::1`. `src:` rules apply to `bind`, which covers listening sockets
and explicit source ports, and to `listen` on a socket that was never
bound, which would otherwise take a port of its own: that counts as a
bind to port 0 on every address. A blocked call fails with "Network is
unreachable" (EACCES for bind and listen)
and a line on stderr names the address, which helps when wrapping a
program whose network needs are not documented.

    sand --net host --allow :53 --allow :443 pip install requests
    sand --net host --block :25 ./app
    sand --net host --allow 10.0.0.0/8 --block 10.1.0.0/16 ./app

How it works: the command runs under a seccomp filter that hands
`connect`, `bind`, `listen`, `sendmsg`, `sendmmsg` and address-carrying
`sendto` to the sandbox's init process. Init takes a handle on the command's
socket, copies the address out of the command's memory, checks the
rules and performs the call itself. Because the kernel never re-reads
the command's memory, a thread changing the address after the check
gains nothing. Plain `send`, `write` and all receives are not trapped.
A call that may block, such as `connect` on a blocking socket, is
performed by a short-lived child of init, so a hung peer does not hold
up the other sockets.

The kernel allows one such filter per process chain, so a filtered
sandbox cannot run inside another filtered sandbox. The inner one
refuses with a message; an unfiltered inner sandbox still runs under
the outer rules.

## Nesting

`sand` inside `sand` works, as long as the binary is visible
inside (it is if it is installed under `/usr`, or pass `--ro` for it).
Each level can only restrict further. The inner `sand` only sees
profiles if `~/.config/sand` is passed in with `--ro`.

## Tests

    make test                    # or ./test.sh [NAME...]

`test.sh` runs the built binary on small cases and prints one line per
case. The network cases need `python3` for a local listener.

## Not covered

Things this tool does not do today. Some are planned, some are out of
scope by design.

Resource limits. Nothing stops a fork bomb, a memory hog or a runaway
CPU loop, and nothing limits how much a `--rw` directory or `/tmp` can
grow. There is no wall-clock timeout either; use `timeout(1)` in front.
Real limits need cgroups, which an unprivileged process only gets when
the system delegates a cgroup to it.

Domain-based network rules. Rules match addresses, not names. Filtering
by domain needs a resolver inside the sandbox that only answers allowed
names and feeds the resulting addresses into the rules.

Inbound traffic. With `--net host`, `src:` rules decide which ports a
program may listen on, but anyone who can reach the host can connect
to a permitted port. There is no filtering by peer address on accept
or on UDP receive.

Root inside. The command runs as your own user, not as root in a user
namespace. `apt install`, `chown` to another user, `mknod`, mounting
filesystems and any setuid program do not work. Files owned by other
users appear as owned by nobody and keep their permissions.

Devices. USB, GPUs, audio, cameras and serial ports are simply absent,
which is the safe default, but there is no flag to let one in. A
`--dev PATH` for a device node would be the natural extension.

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
kernel exploit, and there is no general seccomp syscall allowlist.

Linux only. Namespaces, seccomp and `pidfd_getfd` are Linux features.
WSL2 works, WSL1 does not.
