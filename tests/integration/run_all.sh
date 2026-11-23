#!/bin/bash
# run_all.sh — runs every integration and failure test (i*.sh, x*.sh) and
# prints a summary. Exit status is non-zero if any test failed.
cd "$(dirname "$0")" || exit 1
passed=0
failed=()
for t in i[0-9]*.sh x[0-9]*.sh; do
    [ -f "$t" ] || continue
    if timeout 120 bash "$t"; then
        passed=$((passed + 1))
    else
        failed+=("$t")
        [ -n "${FAIL_FAST:-}" ] && break
    fi
done
echo "========================================"
echo "integration: $passed passed, ${#failed[@]} failed"
for t in "${failed[@]}"; do echo "  failed: $t"; done
[ ${#failed[@]} -eq 0 ]
