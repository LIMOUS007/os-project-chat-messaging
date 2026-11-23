#!/bin/bash
# run.sh BUILD_DIR — starts a server from BUILD_DIR, runs the stress bots,
# stops the server with SIGINT, verifies the database. Used by `make stress`
# (build/rel) and `make tsan` (build/tsan): for tsan, any ThreadSanitizer
# report in the server or bot output fails the test.
set -u
BUILD=${1:-build/rel}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d /tmp/chatstress.XXXXXX)
trap 'kill $SERVER 2>/dev/null; rm -rf "$WORK"' EXIT

PORT=$((20000 + RANDOM % 30000))
"$ROOT/$BUILD/server" --port "$PORT" --db "$WORK/chat.db" \
    --users "$ROOT/tests/stress/users_stress.txt" > "$WORK/server.log" 2>&1 &
SERVER=$!
for _ in $(seq 50); do grep -q listening "$WORK/server.log" && break; sleep 0.1; done

status=0
"$ROOT/$BUILD/stress" "$PORT" 2>&1 | tee "$WORK/stress.log" || status=1
grep -q "stress: PASS" "$WORK/stress.log" || status=1

kill -INT "$SERVER"
wait "$SERVER" || { echo "server exited with an error"; status=1; }
"$ROOT/$BUILD/stress" --verify "$WORK/chat.db" || status=1

if grep -q "ThreadSanitizer" "$WORK/server.log" "$WORK/stress.log"; then
    echo "ThreadSanitizer reported problems:"
    grep -A30 "WARNING: ThreadSanitizer" "$WORK/server.log" | head -80
    status=1
fi
tail -3 "$WORK/server.log"
[ $status -eq 0 ] && echo "STRESS PASS" || echo "STRESS FAIL"
exit $status
