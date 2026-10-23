#!/bin/bash
# Test suite for sand. Runs the binary next to this file (or $SAND) on
# small cases in a scratch directory and reports ok/FAIL per case.
#
#     make test          or     ./test.sh [NAME...]
#
# With names only matching tests run. Needs bash, python3 for the
# listener and terminal cases (skipped otherwise), and unprivileged
# user namespaces.

set -u
cd "$(dirname "$0")" || exit 1
SAND=${SAND:-$PWD/sand}
SAND=$(realpath "$SAND") || exit 1
SANDNET=${SANDNET:-$PWD/sand-net}

T=$(mktemp -d) || exit 1
trap 'kill $LISTENER $HOLDER 2>/dev/null; rm -rf "$T"' EXIT
WORK=$T/work
mkdir -p "$WORK/sub" "$T/other" "$T/cfg/sand"
echo hello > "$WORK/file"
echo other > "$T/other/file"
export XDG_CONFIG_HOME=$T/cfg

pass=0
fail=0
skip=0
only=("$@")

# Runs sand from $WORK, merging stderr into stdout; sets $out and $status.
run() {
    out=$(cd "$WORK" && "$SAND" "$@" 2>&1)
    status=$?
}

selected() {
    local name=$1 n
    [ ${#only[@]} -eq 0 ] && return 0
    for n in "${only[@]}"; do
        [[ $name == *"$n"* ]] && return 0
    done
    return 1
}

ok() { pass=$((pass + 1)); echo "ok   $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1"; echo "     $2"; }
skipped() { skip=$((skip + 1)); echo "skip $1 ($2)"; }

# expect NAME STATUS PATTERN ARGS...: run sand with ARGS and check the
# exit status and that the output matches the glob PATTERN ("*" = any).
expect() {
    local name=$1 want=$2 pattern=$3
    shift 3
    selected "$name" || return 0
    run "$@"
    if [ "$status" != "$want" ]; then
        bad "$name" "exit $status, wanted $want; output: $out"
        return
    fi
    if [[ $out != $pattern ]]; then
        bad "$name" "output: $out; wanted: $pattern"
        return
    fi
    ok "$name"
}

profile() {
    printf '%s\n' "${@:2}" > "$T/cfg/sand/$1"
}

# --- filesystem -------------------------------------------------------

expect "cwd visible read-only" 1 "hello*Read-only file system" \
    sh -c 'cat file; touch new'
expect "--rw . makes cwd writable" 0 "written" \
    --rw . sh -c 'echo x > new && echo written'
expect "--rw subdir leaves parent read-only" 1 "sub ok*Read-only file system" \
    --rw sub sh -c 'touch sub/x && echo sub ok; touch top'
expect "--ro adds a path" 0 "other" \
    --ro "$T/other" cat "$T/other/file"
expect "--rw adds a writable path" 0 "ok" \
    --rw "$T/other" sh -c 'touch '"$T"'/other/x && echo ok'
expect "paths outside are absent" 1 "*No such file*" \
    cat "$T/other/file"
expect "--hide empties a directory" 0 "0" \
    --ro "$T/other" --hide "$T/other" sh -c 'ls -A '"$T"'/other | wc -l'
expect "--hide empties a file" 0 "0" \
    --hide file sh -c 'wc -c < file'
expect "later path flag wins" 0 "ok" \
    --ro sub --rw sub sh -c 'touch sub/x && echo ok'
touch "$T/marker"
expect "/tmp is fresh and writable" 0 "ok" \
    sh -c 'test ! -e '"$T"'/marker && touch /tmp/x && echo ok'
expect "no /home or /var" 0 "gone" \
    sh -c 'test ! -e /home && test ! -e /var && echo gone'
expect "system dirs read-only" 1 "*Read-only file system" \
    sh -c 'touch /usr/x'
expect "refuses root cwd" 1 "*refusing*" \
    --rw / true

# --- environment and process ------------------------------------------

export SECRET=fromhost
expect "host env is dropped" 0 "SAND=1 SECRET=" \
    sh -c 'echo SAND=$SAND SECRET=$SECRET'
expect "--env NAME passes a value" 0 "fromhost" \
    --env SECRET sh -c 'echo $SECRET'
expect "--env NAME=VALUE sets one" 0 "set" \
    --env SECRET=set sh -c 'echo $SECRET'
expect "--env replaces a kept name, once" 0 "HOME=/elsewhere" \
    --env HOME=/elsewhere sh -c 'env | grep ^HOME='
expect "later --env replaces an earlier one" 0 "SECRET=two" \
    --env SECRET=one --env SECRET=two sh -c 'env | grep ^SECRET='
unset SECRET
expect "own pid namespace" 0 "small" \
    sh -c 'test $$ -lt 10 && echo small'
expect "no new privs" 0 "NoNewPrivs:*1" \
    grep NoNewPrivs /proc/self/status
expect "exit status passes through" 7 "" \
    sh -c 'exit 7'
expect "missing command gives 127" 127 "*No such file*" \
    no-such-command
expect "unknown flag gives usage" 2 "usage:*" \
    --bogus true
expect "-- still separates the command" 0 "ok" \
    -- echo ok
expect "-- allows a command starting with -" 127 "*-weird*" \
    -- -weird
expect "flag after command belongs to it" 0 "--rw" \
    echo --rw

# Runs sand in the background without a terminal, sends it SIG after a
# moment, waits for it; sets $status and $survivors (processes left
# running the marker command).
signal_case() {
    local sig=$1 marker=$2
    shift 2
    (cd "$WORK" && exec "$SAND" "$@" sh -c "exec sleep $marker") </dev/null >/dev/null 2>&1 &
    local pid=$!
    sleep 0.5
    kill -"$sig" "$pid"
    wait "$pid" 2>/dev/null
    status=$?
    sleep 0.5
    survivors=$(pgrep -f "sleep $marker" | wc -l)
    pkill -f "sleep $marker" 2>/dev/null
}

if selected "SIGINT without a terminal reaches the command"; then
    signal_case INT 31338
    if [ "$status" = 130 ] && [ "$survivors" = 0 ]; then ok "SIGINT without a terminal reaches the command"
    else bad "SIGINT without a terminal reaches the command" "exit $status, $survivors process(es) survived"; fi
fi
if selected "SIGKILL on sand ends the sandbox"; then
    signal_case KILL 31337
    if [ "$survivors" = 0 ]; then ok "SIGKILL on sand ends the sandbox"
    else bad "SIGKILL on sand ends the sandbox" "$survivors process(es) survived"; fi
fi

# --- network ----------------------------------------------------------

expect "--net none has only loopback" 0 "lo" \
    sh -c 'cut -d: -f1 /proc/net/dev | tail -n +3 | tr -d " "'
expect "loopback is up" 0 "refused" \
    bash -c '{ exec 3<>/dev/tcp/127.0.0.1/1; } 2>&1 | grep -q refused && echo refused'

LISTENER=
PORT=
if command -v python3 >/dev/null; then
    python3 -c '
import socket, sys
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen()
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
held = []
while True:
    c, _ = s.accept()
    held.append(c)
' "$T/port" &
    LISTENER=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -s "$T/port" ] && break
        sleep 0.2
    done
    PORT=$(cat "$T/port" 2>/dev/null)
fi
if [ -n "${PORT:-}" ]; then
    connect="exec 3<>/dev/tcp/127.0.0.1/$PORT && echo connected"
    expect "--net host reaches a local port" 0 "connected" \
        --net host bash -c "$connect"
else
    skipped "network host case" "python3 not found"
fi

# --- named networks ---------------------------------------------------
# sand-net needs root and ip/nft to build one; what can be tested here
# is the join: a holder process makes a user namespace it owns with a
# network namespace inside, listening on a port there, and a directory
# points at its namespace files the way /var/lib/sand/net/NAME would.

HOLDER=
NPORT=
if command -v python3 >/dev/null; then
    python3 - "$T/holder" <<'PY' &
import ctypes, fcntl, os, socket, struct, sys
libc = ctypes.CDLL(None, use_errno=True)
uid, gid = os.getuid(), os.getgid()
if libc.unshare(0x10000000) != 0:
    sys.exit("unshare user: " + os.strerror(ctypes.get_errno()))
open("/proc/self/setgroups", "w").write("deny")
open("/proc/self/gid_map", "w").write("%d %d 1" % (gid, gid))
open("/proc/self/uid_map", "w").write("%d %d 1" % (uid, uid))
if libc.unshare(0x40000000) != 0:
    sys.exit("unshare net: " + os.strerror(ctypes.get_errno()))
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
flags = struct.unpack("16sH14s", fcntl.ioctl(s, 0x8913, struct.pack("16sH14s", b"lo", 0, b"")))[1]
fcntl.ioctl(s, 0x8914, struct.pack("16sH14s", b"lo", flags | 1, b""))
l = socket.socket()
l.bind(("127.0.0.1", 0))
l.listen()
open(sys.argv[1], "w").write("%d %d" % (os.getpid(), l.getsockname()[1]))
while True:
    c, _ = l.accept()
    c.send(b"inside\n")
    c.close()
PY
    HOLDER=$!
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -s "$T/holder" ] && break
        sleep 0.2
    done
    read -r HPID NPORT < "$T/holder" 2>/dev/null
fi
if [ -n "${NPORT:-}" ]; then
    mkdir -p "$T/netA" "$T/stale"
    ln -s "/proc/$HPID/ns/user" "$T/netA/user"
    ln -s "/proc/$HPID/ns/net" "$T/netA/net"
    echo "nameserver 10.44.1.1" > "$T/netA/resolv.conf"
    touch "$T/stale/user" "$T/stale/net"
    inside="exec 3<>/dev/tcp/127.0.0.1/$NPORT && head -1 <&3"
    expect "--net DIR joins the network" 0 "inside" \
        --net "$T/netA" bash -c "$inside"
    expect "a joined network has no capabilities" 0 "CapEff:*0000000000000000" \
        --net "$T/netA" grep CapEff /proc/self/status
    expect "a joined network brings its resolv.conf" 0 "nameserver 10.44.1.1" \
        --net "$T/netA" cat /etc/resolv.conf
    expect "net DIR in a profile" 0 "inside" \
        --ro "$T/netA" --net "$T/netA" bash -c "$inside"
    profile joined "net $T/netA"
    expect "net in a profile joins" 0 "inside" \
        -p joined bash -c "$inside"
    expect "--net none after a network is empty again" 1 "*refused*" \
        -p joined --net none bash -c "$inside"
    expect "a stale network is refused" 1 "*not a namespace*sand-net up*" \
        --net "$T/stale" true
    expect "--net NAME looks under /var/lib/sand/net" 1 "*/var/lib/sand/net/nosuch/user*" \
        --net nosuch true
else
    skipped "named network cases" "python3 not found"
fi

expect "sand-net list without state" 0 "" \
    --ro "$SANDNET" "$SANDNET" list
expect "sand-net create needs root" 1 "*needs root*" \
    --ro "$SANDNET" "$SANDNET" create x
expect "sand-net without a command gives usage" 2 "usage:*" \
    --ro "$SANDNET" "$SANDNET"

# Rules become the nft table; sand-net nft prints it from the configs,
# which the tests write by hand under their own state directory.
mkdir -p "$T/state/work" "$T/state/open" "$T/badstate/bad" "$T/nostate"
printf 'owner %d\nindex 1\ndns 1.1.1.1\ndns 2606:4700::1111\nallow :443\nblock 10.0.0.0/8\nallow [2001:db8::1]:53\nallow 192.168.1.0/24:8000-9000\n' "$(id -u)" > "$T/state/work/config"
printf 'owner %d\nindex 2\ndns 9.9.9.9\nblock :25\n' "$(id -u)" > "$T/state/open/config"
printf 'owner %d\nindex 1\nallow 1.2.3.4:99999\n' "$(id -u)" > "$T/badstate/bad/config"
cat > "$T/nft.expected" <<'NFT'
add table inet sand
flush table inet sand
table inet sand {
    chain forward {
        type filter hook forward priority 0; policy accept;
        ct state established,related accept
        iifname "sand-*" oifname "sand-*" reject
        iifname "sand-open" jump net_open
        iifname "sand-work" jump net_work
    }
    chain input {
        type filter hook input priority 0; policy accept;
        ct state established,related accept
        iifname "sand-open" jump net_open
        iifname "sand-work" jump net_work
    }
    chain net_open {
        ip daddr 9.9.9.9 meta l4proto { tcp, udp } th dport 53 accept
        meta l4proto { tcp, udp } th dport 25 reject
        accept
    }
    chain net_work {
        ip daddr 1.1.1.1 meta l4proto { tcp, udp } th dport 53 accept
        ip6 daddr 2606:4700::1111 meta l4proto { tcp, udp } th dport 53 accept
        ip daddr 192.168.1.0/24 meta l4proto { tcp, udp } th dport 8000-9000 accept
        ip6 daddr 2001:db8::1/128 meta l4proto { tcp, udp } th dport 53 accept
        ip daddr 10.0.0.0/8 reject
        meta l4proto { tcp, udp } th dport 443 accept
        reject
    }
    chain postrouting {
        type nat hook postrouting priority 100; policy accept;
        ip saddr 10.44.2.0/30 oifname != "sand-open" masquerade
        ip saddr 10.44.1.0/30 oifname != "sand-work" masquerade
    }
}
NFT
if selected "sand-net nft translates the rules"; then
    run --ro "$SANDNET" --ro "$T/state" --env "SAND_NET_STATE=$T/state" "$SANDNET" nft
    if [ "$status" = 0 ] && [ "$out" = "$(cat "$T/nft.expected")" ]; then
        ok "sand-net nft translates the rules"
    else
        bad "sand-net nft translates the rules" "exit $status; output: $out"
    fi
fi
expect "sand-net nft without networks prints nothing" 0 "" \
    --ro "$SANDNET" --ro "$T/nostate" --env "SAND_NET_STATE=$T/nostate" "$SANDNET" nft
expect "sand-net refuses a bad rule in a config" 1 "*bad port: 99999*" \
    --ro "$SANDNET" --ro "$T/badstate" --env "SAND_NET_STATE=$T/badstate" "$SANDNET" nft
expect "sand-net list shows the rules" 0 "open*down*10.44.2.0/30*
*block :25
work*down*10.44.1.0/30*
*allow :443
*block 10.0.0.0/8
*allow ?2001:db8::1?:53
*allow 192.168.1.0/24:8000-9000" \
    --ro "$SANDNET" --ro "$T/state" --env "SAND_NET_STATE=$T/state" "$SANDNET" list

# --- nesting ----------------------------------------------------------

expect "sand inside sand" 0 "inner" \
    --ro "$SAND" "$SAND" echo inner
expect "inner cannot widen" 1 "*Read-only file system" \
    --ro "$SAND" "$SAND" --rw . sh -c 'touch x'

# --- terminal ---------------------------------------------------------
# Drives an interactive bash on a pseudo-terminal. Steps: ">TEXT" types
# a line, "^Z" and "^C" send the control character, "<TEXT" waits for
# TEXT to appear, "!TEXT" says TEXT must never appear. The transcript
# is printed on failure.
tty_session() {
    python3 - "$@" <<'PY'
import os, pty, select, signal, sys, time

steps = sys.argv[1:]
pid, fd = pty.fork()
if pid == 0:
    os.environ["PS1"] = "P> "
    os.environ["TERM"] = "dumb"
    os.execvp("bash", ["bash", "--norc", "--noprofile", "-i"])

out = b""
pos = 0
forbidden = []

def read_some(timeout):
    global out
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return False
    try:
        data = os.read(fd, 4096)
    except OSError:
        return False
    if not data:
        return False
    out += data
    return True

def finish(code, msg):
    if msg:
        print(msg)
        print("transcript:")
        print(out.decode("utf-8", "replace").replace("\r", ""))
    os.close(fd)
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
    sys.exit(code)

def expect(text):
    global pos
    deadline = time.time() + 10
    while out.find(text.encode(), pos) < 0:
        if time.time() > deadline:
            finish(1, "timed out waiting for: " + text)
        read_some(0.2)
    pos = out.find(text.encode(), pos) + len(text)

expect("P> ")
for step in steps:
    if step == "^Z" or step == "^C":
        time.sleep(0.5)
        if step == "^Z":
            os.write(fd, b"\x1a")
        else:
            os.write(fd, b"\x03")
    elif step[0] == ">":
        os.write(fd, step[1:].encode() + b"\n")
    elif step[0] == "<":
        expect(step[1:])
    elif step[0] == "!":
        forbidden.append(step[1:])
os.write(fd, b"exit\n")
deadline = time.time() + 5
while time.time() < deadline and read_some(0.2):
    pass
for text in forbidden:
    if text.encode() in out:
        finish(1, "unwanted output: " + text)
finish(0, "")
PY
}

tty_case() {
    local name=$1
    shift
    selected "$name" || return 0
    if out=$(tty_session "$@" 2>&1); then
        ok "$name"
    else
        bad "$name" "$out"
    fi
}

if command -v python3 >/dev/null; then
    printf 'tpgid=$(cut -d" " -f8 /proc/self/stat)\n[ "$tpgid" != 0 ] && echo visible || echo invisible:$tpgid\n' > "$T/tpgid.sh"
    tty_case "tty: foreground group is visible inside" \
        ">$SAND --ro $T sh $T/tpgid.sh" "<visible"
    tty_case "tty: interactive dash exits quietly" \
        ">$SAND -- sh -i" '<$ ' ">exit" ">echo do''ne:\$?" "<done:0" \
        "!tty process group" "!can't access tty"
    tty_case "tty: ctrl-z stops the job, fg resumes it, ctrl-c ends it" \
        ">$SAND -- sleep 100" "^Z" "<Stopped" ">jobs" "<sleep 100" \
        ">fg" "<sleep 100" "^C" ">echo rc=\$?" "<rc=130"
    tty_case "tty: bg continues the job without the terminal" \
        ">$SAND -- sleep 100" "^Z" "<Stopped" ">bg" "<sleep 100" ">jobs" "<Running" \
        ">kill %1; wait %1; echo rc=\$?" "<rc=143"
    tty_case "tty: background start leaves the shell its terminal" \
        ">$SAND -- sleep 100 &" ">echo outer-alive" "<outer-alive" \
        ">kill %1; wait %1; echo rc=\$?" "<rc=143"
    tty_case "tty: background job reading the terminal stops, fg feeds it" \
        ">$SAND -- sh -c 'read x; echo got:\$x' &" ">echo outer-alive" "<outer-alive" \
        ">wait" "<Stopped" ">fg" ">hello" "<got:hello"
    tty_case "tty: inner shell job control" \
        ">$SAND -- bash --norc -i" ">sleep 100 &" ">jobs" "<Running" \
        ">kill %1; wait; echo jo''bs-ok" "<jobs-ok" ">exit" ">echo do''ne:\$?" "<done:0"
else
    skipped "terminal cases" "python3 not found"
fi

# --- profiles ---------------------------------------------------------

profile net "# comment" "net host" "" "env NET=1"
profile lan "profile net" "env LAN=1"
profile dev "profile lan" "--rw ." "env FOO=bar" "ro ~"
profile loop1 "profile loop2"
profile loop2 "profile loop1"
profile bad "bogus 1"
profile novalue "rw"

expect "-p loads a profile" 0 "written" \
    -p dev sh -c 'echo x > new && echo written'
expect "--profile is the long form" 0 "bar" \
    --profile dev sh -c 'echo $FOO'
expect "profile includes another" 0 "lo eth*" \
    -p dev sh -c 'cut -d: -f1 /proc/net/dev | tail -n +3 | tr -d " " | tr "\n" " "'
expect "~ in a profile path" 0 "home" \
    -p dev sh -c 'test -d "$HOME" && echo home'
expect "profile comments and blank lines" 0 "ok" \
    -p net echo ok
expect "flag after profile wins" 0 "lo" \
    -p dev --net none sh -c 'cut -d: -f1 /proc/net/dev | tail -n +3 | tr -d " "'
expect "missing profile" 1 "*profile*missing*No such file*" \
    -p missing true
expect "profile loop" 1 "*nested too deep*" \
    -p loop1 true
expect "unknown flag in profile" 1 "*bad:1: unknown flag bogus" \
    -p bad true
expect "flag without value in profile" 1 "*novalue:1: rw needs a value" \
    -p novalue true
expect "profile name with a slash" 1 "*plain name*" \
    -p ../x true
expect "-p without a name gives usage" 2 "usage:*" \
    -p
expect "no default profile is fine" 0 "none" \
    sh -c 'echo ${FOO:-none}'
profile default "env FOO=default"
expect "default profile is loaded first" 0 "default" \
    sh -c 'echo $FOO'
expect "command line overrides default" 0 "FOO=cli" \
    --env FOO=cli sh -c 'env | grep ^FOO='
expect "--hide of a missing path is a no-op" 0 "ok" \
    --hide "$T/nowhere" echo ok
expect "--ro of a missing path fails" 1 "*nowhere*No such file*" \
    --ro "$T/nowhere" true

# --- shipped profiles -------------------------------------------------

cp profiles/* "$T/cfg/sand/"
for f in profiles/*; do
    expect "shipped profile ${f#profiles/} loads" 0 "ok" \
        -p "${f#profiles/}" echo ok
done

# --- summary ----------------------------------------------------------

echo
echo "$pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
