#!/bin/bash
# I12: SIGTERM (what `docker stop` sends) gives the same result as I11.
SIGNAL=TERM TEST_NAME=i12_sigterm exec bash "$(dirname "$0")/i11_sigint.sh"
