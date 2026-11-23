#!/bin/bash
# lib.sh — helpers for the integration tests (SPEC §11.2).
#
# Each test script sources this file. Every test gets a fresh server on a
# random port with a temporary database. Clients run in the background, read
# their commands from a FIFO and write their output to a log file; the test
# then checks the logs with grep.
#
#   start_server [server_binary]       start a server (waits for "listening")
#   start_client NAME                  start a client process called NAME
#   cmd NAME "line"                    type a line into NAME's client
#   login NAME [USER]                  start_client + connect + wait for "Connected"
#   expect NAME REGEX [SECONDS]        wait until NAME's log matches
#   expect_not NAME REGEX              NAME's log must NOT match
#   expect_server REGEX [SECONDS]      wait until the server log matches
#   count NAME REGEX                   number of matching lines
#   end_input NAME                     close NAME's stdin (client shuts down)
#   wait_exit NAME CODE [SECONDS]      NAME must exit with CODE
#   finish                             print PASS/FAIL and exit
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SERVER_BIN=${SERVER_BIN:-$ROOT/build/rel/server}
TEST_SERVER_BIN=$ROOT/build/test/server
CLIENT_BIN=${CLIENT_BIN:-$ROOT/build/rel/client}
RAWBOT=$ROOT/build/rel/rawbot

# FIFOs do not work on Windows bind mounts, so everything lives in /tmp.
WORK=$(mktemp -d /tmp/chattest.XXXXXX)
PORT=
SERVER_PID=
FAILED=0
TEST_NAME=${TEST_NAME:-$(basename "$0" .sh)}
declare -A CLIENT_PID CLIENT_FD

cleanup() {
    for name in "${!CLIENT_FD[@]}"; do
        eval "exec ${CLIENT_FD[$name]}>&-" 2>/dev/null
    done
    for name in "${!CLIENT_PID[@]}"; do
        kill "${CLIENT_PID[$name]}" 2>/dev/null
    done
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "  FAIL: $*"
    FAILED=1
}

printf '%s\n' alice bob carol dave eve frank grace > "$WORK/users.txt"

start_server() {
    local bin=${1:-$SERVER_BIN}
    for _ in 1 2 3 4 5; do
        PORT=$((20000 + RANDOM % 30000))
        "$bin" --port "$PORT" --db "$WORK/chat.db" --users "$WORK/users.txt" \
            > "$WORK/server.log" 2>&1 &
        SERVER_PID=$!
        for _ in $(seq 30); do
            grep -q "listening" "$WORK/server.log" 2>/dev/null && return 0
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill "$SERVER_PID" 2>/dev/null   # port taken? try another
        wait "$SERVER_PID" 2>/dev/null
    done
    fail "server did not start"
    cat "$WORK/server.log"
    exit 1
}

start_client() {
    local name=$1
    mkfifo "$WORK/$name.in"
    "$CLIENT_BIN" < "$WORK/$name.in" > "$WORK/$name.log" 2>&1 &
    CLIENT_PID[$name]=$!
    local fd
    exec {fd}> "$WORK/$name.in"      # keep the FIFO open: no EOF until end_input
    CLIENT_FD[$name]=$fd
}

cmd() {
    echo "$2" >&"${CLIENT_FD[$1]}"
}

login() {
    local name=$1 user=${2:-$1}
    start_client "$name"
    cmd "$name" "connect 127.0.0.1:$PORT $user"
    expect "$name" "Connected as $user"
}

# expect NAME REGEX [SECONDS] — the log of NAME must match REGEX in time.
expect() {
    local name=$1 regex=$2 timeout=${3:-5}
    local log="$WORK/$name.log"
    [ "$name" = server ] && log="$WORK/server.log"
    local tries=$((timeout * 10))
    for _ in $(seq "$tries"); do
        grep -qE -- "$regex" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    fail "$name: expected /$regex/"
    echo "  ---- $name log:"
    tail -20 "$log" | cut -c1-160 | sed 's/^/  | /'
    return 1
}

expect_server() { expect server "$1" "${2:-5}"; }

expect_not() {
    local name=$1 regex=$2
    local log="$WORK/$name.log"
    [ "$name" = server ] && log="$WORK/server.log"
    if grep -qE -- "$regex" "$log" 2>/dev/null; then
        fail "$name: did not expect /$regex/"
        tail -20 "$log" | cut -c1-160 | sed 's/^/  | /'
    fi
}

count() {
    local log="$WORK/$1.log"
    [ "$1" = server ] && log="$WORK/server.log"
    grep -cE -- "$2" "$log" 2>/dev/null || true
}

end_input() {
    eval "exec ${CLIENT_FD[$1]}>&-"
    unset "CLIENT_FD[$1]"
}

# wait_exit NAME CODE [SECONDS]
wait_exit() {
    local name=$1 want=$2 timeout=${3:-5}
    local pid=${CLIENT_PID[$name]}
    for _ in $(seq $((timeout * 10))); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        fail "$name did not exit"
        return 1
    fi
    wait "$pid"
    local code=$?
    unset "CLIENT_PID[$name]"
    [ "$code" -eq "$want" ] || fail "$name exited with $code, expected $want"
}

# Waits for the server process to exit; checks its exit code.
wait_server_exit() {
    local want=$1 timeout=${2:-5}
    for _ in $(seq $((timeout * 10))); do
        kill -0 "$SERVER_PID" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "server did not exit"
        return 1
    fi
    wait "$SERVER_PID"
    local code=$?
    SERVER_PID=
    [ "$code" -eq "$want" ] || fail "server exited with $code, expected $want"
}

finish() {
    if [ "$FAILED" -eq 0 ]; then
        echo "PASS $TEST_NAME"
    else
        echo "FAIL $TEST_NAME"
        echo "  ---- server log:"
        tail -25 "$WORK/server.log" | cut -c1-160 | sed 's/^/  | /'
    fi
    exit "$FAILED"
}
