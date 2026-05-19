#!/bin/sh
#
# End-to-end smoke for the atomic-rollback demonstration.
#
#   1. GET  /counter                  -- capture the initial value.
#   2. POST /increment-with-failure   -- trigger the deliberate failure.
#   3. GET  /counter                  -- capture the final value.
#
# Asserts: final == initial. The unchanged value is the rollback evidence.
# Prints a transcript and exits 0 on success, non-zero on failure.
#
# Usage: ./smoke.sh [host[:port]]
#   default endpoint: 127.0.0.1:8080

set -eu

ENDPOINT="${1:-127.0.0.1:8080}"
BASE="http://$ENDPOINT"

# Pre-flight: confirm endpoint is reachable.
if ! curl -sS -o /dev/null --connect-timeout 2 --max-time 5 "$BASE/counter"; then
    echo "Cannot reach $BASE/counter -- is the platform running with this example deployed?" >&2
    exit 2
fi

echo "=== atomic-demo smoke against $BASE ==="

# Step 1: capture initial counter.
initial_response=$(curl -sS --max-time 5 "$BASE/counter")
initial=$(printf '%s' "$initial_response" | sed -n 's/^counter=\([0-9][0-9]*\).*/\1/p')

if [ -z "$initial" ]; then
    echo "Step 1 FAILED: GET /counter returned unexpected body:" >&2
    printf '%s\n' "$initial_response" >&2
    exit 1
fi

printf '\nStep 1: GET /counter\n  %s\n' "$initial_response"

# Step 2: POST /increment-with-failure. Expects 200 OK with body that names the
# caught deliberate error. The route catches the error so the dispatch path
# returns instead of crashing; the atomic rollback fires regardless of catch.
printf '\nStep 2: POST /increment-with-failure\n'
post_response=$(curl -sS --max-time 5 -X POST "$BASE/increment-with-failure")
printf '  %s\n' "$post_response"

case "$post_response" in
    *"deliberate-failure-fired"*) ;;
    *)
        echo "Step 2 FAILED: POST body does not contain deliberate-failure-fired" >&2
        exit 1
        ;;
esac

# Step 3: capture final counter.
final_response=$(curl -sS --max-time 5 "$BASE/counter")
final=$(printf '%s' "$final_response" | sed -n 's/^counter=\([0-9][0-9]*\).*/\1/p')

if [ -z "$final" ]; then
    echo "Step 3 FAILED: GET /counter returned unexpected body:" >&2
    printf '%s\n' "$final_response" >&2
    exit 1
fi

printf '\nStep 3: GET /counter (after deliberate failure)\n  %s\n' "$final_response"

# Assertion: final must equal initial. If it advanced, the rollback did not fire.
printf '\n'
if [ "$initial" = "$final" ]; then
    printf '=== PASS: counter unchanged across deliberate-failure increment ===\n'
    printf '         initial=%s   final=%s   (rollback verified)\n' "$initial" "$final"
    exit 0
else
    printf '=== FAIL: counter changed across deliberate-failure increment ===\n' >&2
    printf '         initial=%s   final=%s   (rollback did NOT fire)\n' "$initial" "$final" >&2
    exit 1
fi
