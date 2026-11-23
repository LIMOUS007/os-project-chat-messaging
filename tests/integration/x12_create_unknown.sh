#!/bin/bash
# X12: creategroup #g alice nobody -> rejected, and nothing is created.
source "$(dirname "$0")/lib.sh"
start_server
login alice
cmd alice "creategroup #g bob nobody"
expect alice "Error: unknown user 'nobody'"
cmd alice "mygroups"
expect alice "You are not in any groups\."
cmd alice "creategroup #g bob"               # the name is still free
expect alice "Created #g with alice, bob\."
finish
