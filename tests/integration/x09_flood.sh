#!/bin/bash
# X9: one client floods 10,000 commands without reading the replies: the
# event loop never blocks, other clients stay responsive, and the flooder is
# slowed by backpressure (and disconnected once its output buffer is full).
source "$(dirname "$0")/lib.sh"
start_server
login carol
"$RAWBOT" "$PORT" flood dave 10000 > "$WORK/flood.log" 2>&1 &
FLOOD=$!
sleep 1
for n in 1 2 3; do
    cmd carol "sendmessage alice still here $n"
    expect carol "Stored for alice \(offline\) \(#[0-9]+\)\." 5
done
cmd carol "getuserlist"
expect carol "Online \([12]\)"
wait "$FLOOD"
expect flood "flood sent [0-9]+ of 10000"
cmd carol "getuserlist"
expect carol "Online \(1\): carol"
finish
