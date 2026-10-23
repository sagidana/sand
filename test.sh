#!/bin/bash
# Test suite for sand. Runs the binary next to this file (or $SAND) on
# small cases in a scratch directory and reports ok/FAIL per case.
#
#     make test          or     ./test.sh [NAME...]
#
# With names only matching tests run. Needs bash, python3 for the
# network cases (skipped otherwise), and unprivileged user namespaces.

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

# --- network ----------------------------------------------------------

expect "--net none has only loopback" 0 "lo" \
    sh -c 'cut -d: -f1 /proc/net/dev | tail -n +3 | tr -d " "'
expect "loopback is up" 0 "refused" \
    bash -c '{ exec 3<>/dev/tcp/127.0.0.1/1; } 2>&1 | grep -q refused && echo refused'
expect "rules need --net host" 1 "*need --net host*" \
    --allow :53 true

LISTENER=
PORT=
if command -v python3 >/dev/null; then
    python3 -c '
import socket, sys
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen()
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
while True:
    c, _ = s.accept()
    c.close()
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
    expect "src: rule blocks bind" 1 "*ermission*" \
        --net host --block src:all python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0))'
    expect "filtered inside filtered is refused" 1 "*cannot be used inside*" \
        --net host --block all --ro "$SAND" "$SAND" --net host --block all true
    expect "unfiltered inside filtered keeps outer rules" 1 "*unreachable*" \
        --net host --block all --ro "$SAND" "$SAND" --net host bash -c "$connect"
else
    skipped "network rule cases" "python3 not found"
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
    tty_case "tty: ctrl-z and fg with network rules" \
        ">$SAND --net host --block all -- sleep 100" "^Z" "<Stopped" \
        ">fg" "<sleep 100" "^C" ">echo rc=\$?" "<rc=130"
    tty_case "tty: bg continues the job without the terminal" \
        ">$SAND -- sleep 100" "^Z" "<Stopped" ">bg" "<sleep 100" ">jobs" "<Running" \
        ">kill %1; wait %1; echo rc=\$?" "<rc=143"
    tty_case "tty: inner shell job control" \
        ">$SAND -- bash --norc -i" ">sleep 100 &" ">jobs" "<Running" \
        ">kill %1; wait; echo jo''bs-ok" "<jobs-ok" ">exit" ">echo do''ne:\$?" "<done:0"
else
    skipped "terminal cases" "python3 not found"
fi

# --- profiles ---------------------------------------------------------

profile net "# comment" "net host" "allow :53" "" "allow :443"
profile lan "profile net" "allow 10.0.0.0/8"
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
expect "command line overrides default" 0 "cli" \
    --env FOO=cli sh -c 'echo $FOO'
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
