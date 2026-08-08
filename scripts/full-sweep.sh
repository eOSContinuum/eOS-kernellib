#!/bin/sh
#
# Run the full pre-PR regression sweep end to end: the 32 steps
# scripts/README.md enumerates under "Full regression sweep", executed
# in order with a per-step verdict and one final summary line. This is
# the runnable form of that section; the README keeps the annotated
# enumeration as the explained reference, and the two are maintained
# together (a step added there is added here, same number).
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/full-sweep.sh
#   LPC_EXT_CRYPTO=/path/to/crypto.<ver> DGD_BIN=<dgd> scripts/full-sweep.sh
#   DGD_BIN=<dgd> scripts/full-sweep.sh 15 16 29     # a step subset
#
# Without LPC_EXT_CRYPTO the module-gated steps report SKIP -- the
# documented module-less bar -- and the sweep can still finish PASS
# (with skips named). With the module set, every step runs.
#
# Verdicts come from each step command's own exit status; the command's
# full output is captured under state/full-sweep-step<N>.log and the
# last lines of a failing step are replayed inline. The summary line is
# the machine-readable result:
#
#   FULL-SWEEP PASS                        every selected step passed
#   FULL-SWEEP PASS (N skipped: ...)       passed at the module-less bar
#   FULL-SWEEP FAIL (first: step N)        at least one step failed
#
# Steps 25-27 (the manual deploy-and-probe steps in the README) are
# automated here with the same clean-slate reset the README prescribes:
# stop nothing (this script owns its boots), remove the prior WWW
# deploy and state files, deploy, boot a live server against a
# localized config copy, probe over HTTP, then shut the boot down.
# Expect the full bar to take on the order of fifteen minutes.

set -u

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$REPO_ROOT" || exit 2

DGD_BIN="${DGD_BIN:-dgd}"
if ! command -v "$DGD_BIN" >/dev/null 2>&1 && [ ! -x "$DGD_BIN" ]; then
    echo "full-sweep.sh: DGD_BIN not found: $DGD_BIN" >&2
    exit 2
fi
if [ -n "${LPC_EXT_CRYPTO:-}" ] && [ ! -f "$LPC_EXT_CRYPTO" ]; then
    echo "full-sweep.sh: LPC_EXT_CRYPTO not found: $LPC_EXT_CRYPTO" >&2
    exit 2
fi

# Dedicated smoke-tier ports so this harness coexists with a live
# default-port instance on the same machine (scripts/README.md Port
# allocation on a shared machine). Exported so every sub-script this
# sweep invokes (run-example.sh, drive-verbs-smoke.sh, and so on) picks
# up the same values even when they are the fallback defaults, not an
# explicit override from the invoking shell.
: "${SMOKE_TELNET_PORT:=48023}"
: "${SMOKE_BINARY_PORT:=48080}"
: "${SMOKE_HTTPS_PORT:=48443}"
export SMOKE_TELNET_PORT SMOKE_BINARY_PORT SMOKE_HTTPS_PORT

mkdir -p state

# Step selection: no args = all steps; args = the numbered subset.
SELECTED="${*:-}"

step_selected() {
    [ -z "$SELECTED" ] && return 0
    for _s in $SELECTED; do
        [ "$_s" = "$1" ] && return 0
    done
    return 1
}

# Verdict accounting. POSIX sh has no arrays; accumulate report lines
# in a file and counters in variables.
REPORT="state/full-sweep-report.txt"
: > "$REPORT"
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
FIRST_FAIL=""
SKIPPED_STEPS=""

record() {  # record <num> <verdict> <desc>
    printf 'step %-3s %-5s %s\n' "$1" "$2" "$3" >> "$REPORT"
    case "$2" in
        PASS) PASS_COUNT=$((PASS_COUNT + 1)) ;;
        FAIL) FAIL_COUNT=$((FAIL_COUNT + 1))
              [ -z "$FIRST_FAIL" ] && FIRST_FAIL="$1" ;;
        SKIP) SKIP_COUNT=$((SKIP_COUNT + 1))
              SKIPPED_STEPS="$SKIPPED_STEPS $1" ;;
    esac
}

# run_step <num> <needs-crypto: 0|1> <desc> <command...>
# The command runs with its output captured; verdict is its exit status.
run_step() {
    _num="$1"; _crypto="$2"; _desc="$3"
    shift 3
    step_selected "$_num" || return 0
    if [ "$_crypto" = 1 ] && [ -z "${LPC_EXT_CRYPTO:-}" ]; then
        echo "--- step $_num: $_desc"
        echo "    SKIP (documented: needs LPC_EXT_CRYPTO)"
        record "$_num" SKIP "$_desc (needs LPC_EXT_CRYPTO)"
        return 0
    fi
    _log="state/full-sweep-step$_num.log"
    echo "--- step $_num: $_desc"
    # Module-less steps run with LPC_EXT_CRYPTO unset even when the
    # sweep has it: their profiles assert the crypto-absent stand-downs
    # (and their sentinel counts assume the ceremony phases skip), so an
    # ambient module fails them with MORE passing phases than expected.
    # The unset lives in a subshell so function steps still resolve.
    if [ "$_crypto" = 0 ]; then
        ( unset LPC_EXT_CRYPTO; "$@" ) > "$_log" 2>&1
        _rc=$?
    else
        "$@" > "$_log" 2>&1
        _rc=$?
    fi
    if [ "$_rc" -eq 0 ]; then
        echo "    PASS"
        record "$_num" PASS "$_desc"
    else
        echo "    FAIL (last lines of $_log follow)"
        tail -n 12 "$_log" | sed 's/^/    | /'
        record "$_num" FAIL "$_desc"
    fi
}

# The deploy-boot-probe cycle behind steps 25-27. Owns exactly one dgd
# boot at a time; the trap keeps a failed probe from orphaning it.
HTTP_PID=""
http_teardown() {
    if [ -n "$HTTP_PID" ] && kill -0 "$HTTP_PID" 2>/dev/null; then
        kill "$HTTP_PID" 2>/dev/null
        wait "$HTTP_PID" 2>/dev/null
    fi
    HTTP_PID=""
    pkill -9 -f 'dgd .*state/full-sweep-http\.dgd' 2>/dev/null || true
}
trap http_teardown EXIT INT TERM

clean_slate() {
    rm -rf src/usr/WWW state/snapshot state/snapshot.old state/swap
}

# http_deploy_boot <example-dir> <boot-log>: clean slate, deploy the
# example as the WWW mount, boot a live server on a localized config
# copy, and wait until GET /health-or-any-route answers or 30s elapse.
http_deploy_boot() {
    _example="$1"; _bootlog="$2"
    clean_slate
    cp -R "examples/$_example" src/usr/WWW || return 1
    sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
        -e "s|:[[:space:]]*8023[[:space:]]*\]|: $SMOKE_TELNET_PORT ]|" \
        -e "s|^binary_port[[:space:]]*=[[:space:]]*8080|binary_port	= $SMOKE_BINARY_PORT|" \
        example.dgd > state/full-sweep-http.dgd
    if ! grep -q "$SMOKE_TELNET_PORT" state/full-sweep-http.dgd \
            || ! grep -q "binary_port.*$SMOKE_BINARY_PORT" state/full-sweep-http.dgd; then
        echo "full-sweep.sh: port rewrite did not land in state/full-sweep-http.dgd (sed pattern miss?)" >&2
        return 1
    fi
    "$DGD_BIN" state/full-sweep-http.dgd > "$_bootlog" 2>&1 &
    HTTP_PID=$!
    _i=0
    while [ "$_i" -lt 30 ]; do
        kill -0 "$HTTP_PID" 2>/dev/null || { HTTP_PID=""; return 1; }
        if curl -s -o /dev/null --connect-timeout 1 --max-time 2 \
            "http://127.0.0.1:$SMOKE_BINARY_PORT/"; then
            return 0
        fi
        sleep 1
        _i=$((_i + 1))
    done
    http_teardown
    return 1
}

step25() {
    http_deploy_boot atomic-demo state/full-sweep-step25-boot.log || return 1
    examples/atomic-demo/smoke.sh "127.0.0.1:$SMOKE_BINARY_PORT"
    _rc=$?
    http_teardown
    return "$_rc"
}

step26() {
    http_deploy_boot hot-reload-demo state/full-sweep-step26-boot.log || return 1
    examples/hot-reload-demo/smoke.sh "127.0.0.1:$SMOKE_BINARY_PORT"
    _rc=$?
    http_teardown
    return "$_rc"
}

# Step 30: anchor-capture check (doc hygiene; needs git, no DGD).
# A heading added to a published file can capture a pre-existing
# dangling anchor and silently misdirect its inbound links. For every
# heading this branch adds relative to BASE_REF (default: main),
# compute its GitHub slug and search the tree for '#<slug>' references
# in files the diff does not touch: each hit is a link that predates
# the heading and now silently resolves to it. Review each reported
# capture; an intended new link target added in the same change lives
# in a touched file and is not reported.
anchor_capture_check() {
    _base="${BASE_REF:-main}"
    if ! git rev-parse --verify "$_base" >/dev/null 2>&1; then
        echo "anchor-capture: base ref not found: $_base (set BASE_REF)"
        return 1
    fi
    _headings="state/full-sweep-anchors-added.txt"
    _hits="state/full-sweep-anchors-hits.txt"
    git diff -U0 "$_base" -- '*.md' | sed -n 's/^+#\{1,6\} //p' | sort -u > "$_headings"
    if [ ! -s "$_headings" ]; then
        echo "no headings added vs $_base"
        return 0
    fi
    _touched=$(git diff --name-only "$_base")
    : > "$_hits"
    while IFS= read -r _h; do
        _slug=$(printf '%s' "$_h" | tr '[:upper:]' '[:lower:]' \
            | sed -e 's/[^a-z0-9 _-]//g' -e 's/ /-/g')
        [ -z "$_slug" ] && continue
        grep -rnE --include='*.md' "#${_slug}([^a-z0-9_-]|\$)" . 2>/dev/null \
            | grep -v '^\./\.git/' | while IFS= read -r _line; do
            _file=${_line%%:*}
            _file=${_file#./}
            case "
$_touched
" in
            *"
$_file
"*) ;;  # link lives in a touched file: authored with the heading
            *) printf 'captured anchor: #%s <- %s\n' "$_slug" "$_line" >> "$_hits" ;;
            esac
        done
    done < "$_headings"
    if [ -s "$_hits" ]; then
        cat "$_hits"
        return 1
    fi
    echo "$(wc -l < "$_headings" | tr -d ' ') added heading(s), no captured anchors"
    return 0
}

step27() {
    http_deploy_boot http-app state/full-sweep-step27-boot.log || return 1
    _rc=0
    _health=$(curl -sS --max-time 5 "http://127.0.0.1:$SMOKE_BINARY_PORT/health") || _rc=1
    [ "$_health" = "ok" ] || { echo "GET /health: expected ok, got: $_health"; _rc=1; }
    _status=$(curl -sS --max-time 5 "http://127.0.0.1:$SMOKE_BINARY_PORT/status") || _rc=1
    case "$_status" in
        *objects=*/*callouts=*) echo "$_status" ;;
        *) echo "GET /status: missing count lines: $_status"; _rc=1 ;;
    esac
    _echo=$(curl -sS --max-time 5 -d 'hello' "http://127.0.0.1:$SMOKE_BINARY_PORT/echo") || _rc=1
    [ "$_echo" = "hello" ] || { echo "POST /echo: expected hello, got: $_echo"; _rc=1; }
    _notfound=$(curl -sS -i --max-time 5 "http://127.0.0.1:$SMOKE_BINARY_PORT/no-such-route") || _rc=1
    case "$_notfound" in
        *"404 Not Found"*) ;;
        *) echo "GET /no-such-route: missing 404: $_notfound"; _rc=1 ;;
    esac
    http_teardown
    return "$_rc"
}

# ---- the sweep, in README order ------------------------------------

run_step 1  0 "run-example chat-app"          env DGD_BIN="$DGD_BIN" scripts/run-example.sh chat-app
run_step 2  0 "run-example hot-reload-demo"   env DGD_BIN="$DGD_BIN" scripts/run-example.sh hot-reload-demo
run_step 3  0 "run-example hot-reload-master" env DGD_BIN="$DGD_BIN" scripts/run-example.sh hot-reload-master
run_step 4  0 "run-example merry-app"         env DGD_BIN="$DGD_BIN" scripts/run-example.sh merry-app
run_step 5  0 "run-example signal-app"        env DGD_BIN="$DGD_BIN" scripts/run-example.sh signal-app
run_step 6  0 "run-example upgrade-cascade"   env DGD_BIN="$DGD_BIN" scripts/run-example.sh upgrade-cascade
run_step 7  0 "run-example vault-app"         env DGD_BIN="$DGD_BIN" scripts/run-example.sh vault-app
run_step 8  0 "run-example webauthn-app (module-less)" \
    env DGD_BIN="$DGD_BIN" scripts/run-example.sh webauthn-app
run_step 9  1 "run-example webauthn-app (ceremonies)" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" EXPECTED_OK=26 \
    scripts/run-example.sh webauthn-app
run_step 10 0 "run-example composite-app (module-less)" \
    env DGD_BIN="$DGD_BIN" scripts/run-example.sh composite-app
run_step 11 1 "run-example composite-app (full set)" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" EXPECTED_OK=53 \
    scripts/run-example.sh composite-app
run_step 12 1 "run-example agent-app" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/run-example.sh agent-app
run_step 13 1 "drive-verbs agent-app continuation" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    DEPLOY="agent-app:AgentApp" \
    scripts/drive-verbs-smoke.sh scripts/verbsets/agent-app.verbset
run_step 14 0 "run-example atomic-demo"       env DGD_BIN="$DGD_BIN" scripts/run-example.sh atomic-demo
run_step 15 0 "drive-verbs default verbsets"  env DGD_BIN="$DGD_BIN" scripts/drive-verbs-smoke.sh
run_step 16 0 "drive-verbs module-less console regressions" \
    env DGD_BIN="$DGD_BIN" scripts/drive-verbs-smoke.sh \
    scripts/verbsets/code-eval-baseline.verbset \
    scripts/verbsets/initd-laundering.verbset \
    scripts/verbsets/kvstore-roundtrip.verbset \
    scripts/verbsets/agent-records.verbset
run_step 17 1 "drive-verbs identity-lifecycle" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/drive-verbs-smoke.sh scripts/verbsets/identity-lifecycle.verbset
run_step 18 1 "drive-verbs identity-capability" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/drive-verbs-smoke.sh scripts/verbsets/identity-capability.verbset
run_step 19 1 "drive-verbs webauthn-ceremony" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/drive-verbs-smoke.sh scripts/verbsets/webauthn-ceremony.verbset
run_step 20 1 "drive-verbs identity-recovery" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/drive-verbs-smoke.sh scripts/verbsets/identity-recovery.verbset
run_step 21 1 "drive-verbs agent substrate suites" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" \
    scripts/drive-verbs-smoke.sh \
    scripts/verbsets/agent-auth.verbset \
    scripts/verbsets/agent-console.verbset \
    scripts/verbsets/agent-delegation.verbset
run_step 22 1 "session-smoke" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" scripts/session-smoke.sh
run_step 23 1 "agent-smoke" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" scripts/agent-smoke.sh
run_step 24 0 "base-boot-guard"               env DGD_BIN="$DGD_BIN" scripts/base-boot-guard.sh
run_step 25 0 "atomic-demo HTTP smoke"        step25
run_step 26 0 "hot-reload-demo HTTP smoke"    step26
run_step 27 0 "http-app curl probes"          step27
run_step 28 1 "https-smoke" \
    env DGD_BIN="$DGD_BIN" LPC_EXT_CRYPTO="${LPC_EXT_CRYPTO:-}" scripts/https-smoke.sh
run_step 29 0 "function-index --check"        python3 scripts/gen-function-index.py --check
run_step 30 0 "anchor-capture check"          anchor_capture_check
run_step 31 0 "doc-sentinel-check"            python3 scripts/doc-sentinel-check.py
run_step 32 0 "tutorial-smoke"                env DGD_BIN="$DGD_BIN" scripts/tutorial-smoke.sh

# ---- summary --------------------------------------------------------

echo
echo "=== full-sweep verdicts ==="
cat "$REPORT"
echo

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo "FULL-SWEEP FAIL (first: step $FIRST_FAIL)"
    exit 1
elif [ "$SKIP_COUNT" -gt 0 ]; then
    echo "FULL-SWEEP PASS ($SKIP_COUNT skipped:$SKIPPED_STEPS)"
    exit 0
else
    echo "FULL-SWEEP PASS"
    exit 0
fi
