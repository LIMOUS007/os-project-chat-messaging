#!/bin/bash
# X7: two clients run creategroup #g at the same moment: exactly one
# succeeds, the other gets GROUP_EXISTS (settled by the primary key).
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "creategroup #race carol" &
A=$!
cmd bob "creategroup #race carol" &
B=$!
wait "$A" "$B"
expect alice "(Created #race|already exists)"
expect bob "(Created #race|already exists)"
created=$(( $(count alice "Created #race") + $(count bob "Created #race") ))
exists=$(( $(count alice "group #race already exists") + $(count bob "group #race already exists") ))
[ "$created" -eq 1 ] || fail "expected exactly one success, got $created"
[ "$exists" -eq 1 ] || fail "expected exactly one GROUP_EXISTS, got $exists"
finish
