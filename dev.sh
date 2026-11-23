#!/bin/sh
# dev.sh — run a command inside the gcc:13 container with this folder mounted.
#   ./dev.sh make test        ./dev.sh make tsan
# (seccomp=unconfined lets ThreadSanitizer control its memory layout.)
exec docker run --rm -i --security-opt seccomp=unconfined -v "$(pwd):/src" -w /src gcc:13 "$@"
