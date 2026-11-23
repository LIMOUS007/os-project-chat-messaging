#!/bin/bash
# X3 (and X3b): the server is shut down while clients are connected and
# sending. Everyone gets the broadcast, "Server shutting down..." is the LAST
# line each client prints, everyone exits 0, and the server exits within ~2 s.
source "$(dirname "$0")/lib.sh"
start_server
users=(alice bob carol dave)
for u in "${users[@]}"; do login "$u"; done
# Each client keeps sending to the next one.
for i in "${!users[@]}"; do
    to=${users[$(((i + 1) % 4))]}
    ( for n in $(seq 200); do echo "sendmessage $to busy $n"; done ) >&"${CLIENT_FD[${users[$i]}]}" &
done
sleep 0.5
start=$(date +%s%N)
kill -INT "$SERVER_PID"
wait_server_exit 0 5
elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
[ "$elapsed_ms" -lt 2500 ] || fail "server took ${elapsed_ms} ms to stop"
for u in "${users[@]}"; do
    expect "$u" "^Server shutting down\.\.\.$"
    wait_exit "$u" 0
    last=$(tail -n 1 "$WORK/$u.log")
    [ "$last" = "Server shutting down..." ] || fail "$u: last line was '$last'"
done
finish
