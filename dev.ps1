# dev.ps1 — Windows version of dev.sh: run a command inside the gcc:13 container.
#   .\dev.ps1 make test        .\dev.ps1 make tsan
docker run --rm -i --security-opt seccomp=unconfined -v "${PWD}:/src" -w /src gcc:13 @args
