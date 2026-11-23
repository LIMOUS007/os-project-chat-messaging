#!/bin/bash
# I10: group create / send / fan-out / offline member at login / late joiner
# cannot see old messages / creator delete / creator leave dissolves the group.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "creategroup #proj bob carol"
expect alice "Created #proj with alice, bob, carol\."
cmd bob "creategroup #proj alice"
expect bob "Error: group #proj already exists"
cmd alice "sendgroup #proj standup at 10"
expect alice "Sent to #proj \(#[0-9]+\)\."
expect bob "^Message from alice in #proj: standup at 10$"
expect_not alice "Message from alice in #proj"           # not to the sender

login carol                                               # was offline
expect carol "Message from alice in #proj: standup at 10"

cmd bob "addmember #proj dave"
expect bob "Error: only the creator can add members"
login dave
cmd alice "addmember #proj dave"
expect alice "Added dave to #proj\."
expect dave "\[alice added you to #proj\]"
cmd dave "getmessages #proj"
expect dave "No messages in #proj\."                     # join watermark
cmd bob "sendgroup #proj after dave"
expect dave "Message from bob in #proj: after dave"
cmd dave "getmessages #proj"
expect dave "\-\-\- Messages in #proj \(1\) \-\-\-"
cmd alice "mygroups"
expect alice "Your groups: #proj"

login eve
cmd eve "getmessages #proj"
expect eve "Error: you are not a member of #proj"
cmd eve "sendgroup #nogroup hi"
expect eve "Error: no such group #nogroup"

cmd bob "deletemessages #proj"
expect bob "Error: only the creator can delete #proj history"
cmd alice "deletemessages #proj"
expect alice "Deleted 2 message\(s\) in #proj\."
cmd bob "getmessages #proj"
expect bob "No messages in #proj\."

cmd carol "leavegroup #proj"
expect carol "^Left #proj\.$"
cmd carol "getmessages #proj"
expect carol "Error: you are not a member of #proj"

cmd alice "leavegroup #proj"
expect alice "Left #proj \(group dissolved\)\."
expect bob "\[#proj was dissolved by alice\]"
expect dave "\[#proj was dissolved by alice\]"
cmd bob "mygroups"
expect bob "You are not in any groups\."
finish
