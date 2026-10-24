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

T=$(mktemp -d) || exit 1
trap 'kill $LISTENER 2>/dev/null; rm -rf "$T"' EXIT
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
expect "rules need --net host" 1 "*need --net host*" \
    --allow :53 true
expect "bad rule" 1 "*bad port: 99999*" \
    --net host --allow :99999 true

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
    expect "--block all blocks connect" 1 "*blocked connection to 127.0.0.1 port $PORT*unreachable*" \
        --net host --block all bash -c "$connect"
    expect "--allow one port lets it through" 0 "connected" \
        --net host --allow 127.0.0.1:$PORT bash -c "$connect"
    expect "--allow elsewhere blocks the rest" 1 "*unreachable*" \
        --net host --allow :1 bash -c "$connect"
    expect "later rule wins" 0 "connected" \
        --net host --block all --allow :$PORT bash -c "$connect"
    # A blocking send into a peer that never reads hangs in the broker;
    # a second socket must still be served meanwhile.
    cat > "$T/stall.py" <<'PY'
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
s.connect(("127.0.0.1", int(sys.argv[1])))
while True:
    s.sendmsg([b"x" * 65536])
PY
    expect "a blocking send does not stall the broker" 0 "connected" \
        --net host --block :1 --ro "$T/stall.py" bash -c "python3 $T/stall.py $PORT & sleep 1; timeout 5 bash -c '$connect'; kill \$!"
    expect "src: rule blocks bind" 1 "*ermission*" \
        --net host --block src:all python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0))'
    expect "src: rule blocks listen on an unbound socket" 1 "*blocked bind to 0.0.0.0 port 0*ermission*" \
        --net host --block src:all python3 -c 'import socket; s=socket.socket(); s.listen()'
    expect "src: allow elsewhere blocks an unbound listen" 1 "*ermission*" \
        --net host --allow src:127.0.0.1 python3 -c 'import socket; s=socket.socket(); s.listen()'
    expect "listen on an allowed bound socket passes" 0 "ok" \
        --net host --allow src:127.0.0.1 python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(); print("ok")'
    expect "0.0.0.0 as a destination counts as loopback" 1 "*blocked connection to 0.0.0.0*unreachable*" \
        --net host --block 127.0.0.0/8 bash -c "exec 3<>/dev/tcp/0.0.0.0/$PORT"
    expect "0.0.0.0 passes when loopback is allowed" 0 "connected" \
        --net host --allow 127.0.0.1:$PORT bash -c "exec 3<>/dev/tcp/0.0.0.0/$PORT && echo connected"
    expect "IPv4 mapped in IPv6 is judged as IPv4" 1 "*blocked connection to 127.0.0.1 port $PORT*unreachable*" \
        --net host --block 127.0.0.0/8 python3 -c "import socket; s=socket.socket(socket.AF_INET6); s.connect(('::ffff:127.0.0.1', $PORT))"
    expect "udp sendto is judged" 1 "*blocked connection to 127.0.0.1 port 9*unreachable*" \
        --net host --allow :$PORT python3 -c 'import socket; u=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.sendto(b"x", ("127.0.0.1", 9))'
    expect "udp sendto to an allowed port passes" 0 "1" \
        --net host --allow :$PORT python3 -c "import socket; u=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); print(u.sendto(b'x', ('127.0.0.1', $PORT)))"
    expect "sendmsg on a connected tcp socket passes" 0 "3" \
        --net host --allow :$PORT python3 -c "import socket; t=socket.socket(); t.connect(('127.0.0.1', $PORT)); print(t.sendmsg([b'abc']))"
    expect "filtered inside filtered is refused" 1 "*cannot be used inside*" \
        --net host --block all --ro "$SAND" "$SAND" --net host --block all true
    expect "unfiltered inside filtered keeps outer rules" 1 "*unreachable*" \
        --net host --block all --ro "$SAND" "$SAND" --net host bash -c "$connect"
    profile pinned "net host" "allow 127.0.0.1:$PORT"
    expect "rules in a profile" 0 "connected" \
        -p pinned bash -c "$connect"
    expect "--net none after rules drops them" 1 "*refused*" \
        -p pinned --net none bash -c "$connect"

    # What the filter closes besides addresses.
    expect "io_uring is absent" 0 "Function not implemented" \
        --net host --block :1 python3 -c 'import ctypes, os; l = ctypes.CDLL(None, use_errno=True); l.syscall(425, 8, ctypes.c_void_p(0)); print(os.strerror(ctypes.get_errno()))'
    expect "unknown socket families are refused" 0 "vsock:Address family not supported by protocol sctp:Protocol not supported seqpacket:Socket type not supported raw:Socket type not supported netlink-generic:Protocol not supported socketpair-netlink:Address family not supported by protocol" \
        --net host --block :1 python3 -c '
import socket
out = []
for name, args in [("vsock", (40, 1, 0)), ("sctp", (2, 1, 132)), ("seqpacket", (2, 5, 0)), ("raw", (2, 3, 1)), ("netlink-generic", (16, 3, 16))]:
    try:
        socket.socket(*args)
        out.append(name + ":ok")
    except OSError as e:
        out.append(name + ":" + e.strerror)
try:
    socket.socketpair(16)
    out.append("socketpair-netlink:ok")
except OSError as e:
    out.append("socketpair-netlink:" + e.strerror)
print(" ".join(out))'
    expect "the usual sockets still open" 0 "ok" \
        --net host --block :1 python3 -c '
import socket
for a in [(1, 1, 0), (1, 2, 0), (2, 1, 0), (2, 2, 0), (10, 1, 0), (10, 2, 0), (16, 3, 0)]:
    socket.socket(*a)
try:
    socket.socket(2, 2, 1)
except PermissionError:
    pass                            # icmp needs the gid in net.ipv4.ping_group_range
socket.socketpair()
print("ok")'
    expect "abstract unix sockets are blocked" 1 "*blocked connection to abstract socket @nowhere*refused*" \
        --net host --block :1 python3 -c 'import socket; s = socket.socket(socket.AF_UNIX); s.connect(b"\0nowhere")'
    expect "abstract unix bind is blocked" 1 "*blocked bind to abstract socket @sand-test*ermission*" \
        --net host --block :1 python3 -c 'import socket; s = socket.socket(socket.AF_UNIX); s.bind(b"\0sand-test")'
    expect "unix paths work, relative to the command, with fd passing" 0 "b'hi' b'via-fd' b'stream' b'conn' 600" \
        --net host --rw . --block :1 python3 -c '
import socket, os, array
os.umask(0o177)
srv = socket.socket(socket.AF_UNIX); srv.bind("rel.sock"); srv.listen()
c = socket.socket(socket.AF_UNIX); c.connect("rel.sock"); a, _ = srv.accept()
d = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); d.bind("dg.sock")
e = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
r, w = os.pipe()
e.sendmsg([b"hi"], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [w]))], 0, "dg.sock")
msg, anc, fl, ad = d.recvmsg(10, socket.CMSG_SPACE(4))
fd = array.array("i"); fd.frombytes(anc[0][2]); os.write(fd[0], b"via-fd"); got = os.read(r, 6)
c.sendmsg([b"stream"]); s = a.recv(6)
e.connect("dg.sock"); e.sendmsg([b"conn"]); k = d.recv(4)
mode = oct(os.stat("dg.sock").st_mode)[-3:]
os.unlink("rel.sock"); os.unlink("dg.sock")
print(msg, got, s, k, mode)'
    expect "fd passing to an ip socket is refused" 1 "*not supported*" \
        --net host --allow :$PORT python3 -c "import socket, array; t=socket.socket(); t.connect(('127.0.0.1', $PORT)); t.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [0]))])"
else
    skipped "network rule cases" "python3 not found"
fi

# --- the command below init --------------------------------------------

expect "the command cannot read init" 1 "*ermission denied*" \
    cat /proc/1/environ
expect "the command cannot mount" 1 "*superuser*" \
    sh -c 'mount -t tmpfs none /tmp 2>&1 || exit 1'
expect "the command maps its own uid" 0 "*$(id -u)*$(id -u)*1" \
    cat /proc/self/uid_map
if command -v python3 >/dev/null; then
    expect "the command cannot take init's descriptors" 0 "Operation not permitted" \
        --net host --block :1 python3 -c 'import os, ctypes; l = ctypes.CDLL(None, use_errno=True); l.syscall(438, os.pidfd_open(1), 3, 0); print(os.strerror(ctypes.get_errno()))'
fi

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
    for name in ("PROMPT_COMMAND", "BASH_ENV", "ENV"):
        os.environ.pop(name, None)
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
        ">wait" "<stopped" ">fg" ">hello" "<got:hello"
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
