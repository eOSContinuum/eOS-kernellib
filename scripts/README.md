# Regression harness

**Audience**: a contributor running or extending the headless boot regressions under `scripts/`; assumes DGD is built per `docs/getting-started.md`.

Six scripts drive the platform through boot cycles and assert or measure the result. The four shell scripts resolve `DGD_BIN` (env override, falling back to `dgd` on `PATH`) and refuse to start if a `dgd` instance already holds the ports; `drive-verbs.py` launches nothing — it is the telnet client the others use against an already-running instance.

## run-example.sh

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh <example>
EXPECTED_OK=21 scripts/run-example.sh chat-app   # override the profile's count
```

Deploys the named example fresh under `src/usr/<Deploy>`, runs its boot sequence, then counts `" OK"` lines in the deployed domain's `data/test-result.log` against the expected count; a `FAIL` line or a count mismatch is a non-zero exit. A `selfexit` boot 1 waits (30s cap) for the driver to dump-and-exit on its own; a `timed` boot 1 runs a fixed 6s window then is killed. `Boots >= 2` restarts against `state/snapshot` (the restore boot); `Boots >= 3` adds a third cold boot with no snapshot loaded (the cold-boot negative case). Each example's recipe is one line in the script's `example_profile()` function.

| Example | Deploy | Boots | Boot 1 | Expected OK |
|---|---|---|---|---|
| chat-app | Chat | 3 | selfexit | 20 |
| hot-reload-demo | WWW | 1 | timed | 2 |
| hot-reload-master | Reload | 1 | timed | 3 |
| merry-app | MerryApp | 2 | selfexit | 28 |
| signal-app | SignalApp | 1 | timed | 1 |
| upgrade-cascade | Cascade | 1 | timed | 7 |
| vault-app | MyApp | 1 | timed | 10 |

`atomic-demo` and `http-app` have no profile here -- they verify over live HTTP (`atomic-demo` via its bundled `smoke.sh`, `http-app` via the probe commands in its README).

## drive-verbs.py + scripts/verbsets/

```sh
python3 scripts/drive-verbs.py scripts/verbsets/admin-baseline.verbset
```

Logs into the console over telnet, then drives each block of a verbset file in session order. A verbset block is one `cmd:` line (the verb to send) plus zero or more `expect:` / `absent:` regex lines (`re.search`, `re.MULTILINE`) checked against the response; blocks separate on a blank line, `#` lines are comments. An `expect:` that fails to match, or an `absent:` that matches, both fail the entry; the run exits non-zero on any entry failure. Consecutive entries share one session, so a verbset can express mutate -> verify -> undo -> verify-undo.

The session logs in as `admin` unless the verbset declares otherwise: file-level `user:` / `password:` directives before the first block (overridable with `--user` / `--password`) let a verbset run as a registered non-admin operator -- the login shape that reaches the System console and its lifecycle verbs (`upgrade` and friends; see `docs/admin-console.md` Connecting). Cold boots register no such operator, so a registered-user verbset runs after a provisioning verbset driven as admin: `operator-provision.verbset` grants the operator, `operator-upgrade.verbset` (`user: testop`) then drives the console `upgrade -p` cascade against a deployed upgrade-cascade example.

## drive-verbs-smoke.sh

```sh
scripts/drive-verbs-smoke.sh                  # default: all eight verbsets over vault-app + upgrade-cascade deploys
DEPLOY="<example>:<Mount> ..." scripts/drive-verbs-smoke.sh <verbset ...>
```

Boots the platform headless and drives verbset file(s) with `drive-verbs.py` against the live telnet console, then shuts down. The default run covers admin-baseline, logging-verbs, schema-verbs, dispatcher-verbs, port-labels, tls-cert, operator-provision, and operator-upgrade in one boot, deploying vault-app as the `MyApp` domain and upgrade-cascade as the `Cascade` domain first -- the dispatcher-verbs clone-addressing cycle drives the property-bearing named clone (`MyApp:core:item1`) the vault-app boot driver creates, the operator cycle drives `upgrade -p` against the settled cascade deploy, and neither example self-exits. The clean-slate step also removes the provisioned operator's artifacts (`src/usr/testop/`, the kernel's persisted access list) so every run reasserts the cold-boot registration flow. With explicit verbset arguments nothing is deployed unless `DEPLOY` asks for it; `DEPLOY` -- a space-separated list of `<example>:<Mount>` pairs -- also overrides the default run's deployment. A boot here is always cold, so a `selfexit` example (one whose driver calls `shutdown()` when it finishes) tears the console down before verbs can run -- drive those against a non-selfexit deployment, or rely on the example's own in-application test phases instead.

## https-smoke.sh

```sh
LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
DGD_BIN=/path/to/dgd/bin/dgd scripts/https-smoke.sh
```

Native-TLS end-to-end, covering the binding and its certificate surface in nine phases. Deploys `examples/https-app` as the `WWW` domain and boots with a second binary port and the lpc-ext crypto module loaded but no certificate (the `LPC_EXT_CRYPTO` path is appended as a `modules` line to the generated config -- the checked-in `example.dgd` stays module-less). Phase 1 proves the bootstrap stood down honestly (`tls-cert` status, no HTTPS service); phase 2 generates a throwaway self-signed P-256 certificate under `src/usr/System/data/tls/` (removed after the run) and activates it with `tls-cert reload` -- registration without a restart; phases 3-7 drive the service probes (`GET /health`, negotiated TLS 1.3, `POST /echo` round-trip, 404 route, cleartext refusal); phases 8-9 take a console `snapshot` twice -- idle and with a live established TLS connection held open -- and scan the statedump for the private key in every in-memory representation (DER, PEM base64, raw scalar), with the port registry's `https` label as the scan's positive control. HTTP probes use `openssl s_client`, not curl -- the stock macOS curl's SecureTransport backend cannot speak TLS 1.3; console phases ride `drive-verbs.py` with an ephemeral verbset under `state/`. `HTTPS-SMOKE PASS` is the pass signal.

## base-boot-guard.sh

```sh
MAX_LINES=400 scripts/base-boot-guard.sh   # override the bound (default 400)
```

Boots the platform with no example deployed for a fixed 6s window, then asserts both the boot log and `src/usr/System/log/system.log` stay at or under `MAX_LINES` -- a regression against the atomic-write-storm failure mode (a caught error inside an atomic function re-entering the error manager and looping a file write).

## measure-baseline.py

```sh
DGD_BIN=/path/to/dgd scripts/measure-baseline.py [--sizes 4,12,28] [--requests 200]
```

The timing rig, not a pass/fail gate: boots cold (timed to console-ready), grows the image in steps by parking integer arrays in a scratch object, records the client-observed snapshot pause and the snapshot file size at each step, times a restore boot against the final snapshot, and drives sequential GETs against the deployed http-app for a throughput figure. It writes its own config copy with `sector_size` raised, because the stock build caps `swap_size` at 65535 sectors and the image must fit the swap device. Numbers land in `docs/operations.md` Limits and capacity; re-run there means re-measuring on your machine, not trusting ours.

## Full regression sweep

Run in this order for the complete pre-PR bar. Each line names the command and the pass signal to look for; `<dgd>` is the path to a built DGD binary.

1. `DGD_BIN=<dgd> scripts/run-example.sh chat-app` -- `PASS`, 20 " OK" sentinels across 3 boots (cold selfexit, snapshot restore, cold no-snapshot).
2. `DGD_BIN=<dgd> scripts/run-example.sh hot-reload-demo` -- `PASS`, 2 " OK" sentinels (1 timed boot).
3. `DGD_BIN=<dgd> scripts/run-example.sh hot-reload-master` -- `PASS`, 3 " OK" sentinels (1 timed boot).
4. `DGD_BIN=<dgd> scripts/run-example.sh merry-app` -- `PASS`, 28 " OK" sentinels across 2 boots (cold selfexit, snapshot restore).
5. `DGD_BIN=<dgd> scripts/run-example.sh signal-app` -- `PASS`, 1 " OK" sentinel (1 timed boot).
6. `DGD_BIN=<dgd> scripts/run-example.sh upgrade-cascade` -- `PASS`, 7 " OK" sentinels (1 timed boot).
7. `DGD_BIN=<dgd> scripts/run-example.sh vault-app` -- `PASS`, 10 " OK" sentinels (1 timed boot).
8. `DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh` -- `DRIVE-VERBS PASS` after the eight default verbsets (admin-baseline, logging-verbs, schema-verbs, dispatcher-verbs, port-labels, tls-cert, operator-provision, operator-upgrade) run against the vault-app (MyApp) and upgrade-cascade (Cascade) deploys.
9. `DGD_BIN=<dgd> scripts/base-boot-guard.sh` -- `GUARD PASS`, boot log and `system.log` both at or under 400 lines (`MAX_LINES`, no example deployed).
10. Deploy atomic-demo (`cp -R examples/atomic-demo src/usr/WWW`), boot against `example.dgd`, then run `examples/atomic-demo/smoke.sh` -- `=== PASS: counter unchanged across deliberate-failure increment ===`.
11. Deploy hot-reload-demo (`cp -R examples/hot-reload-demo src/usr/WWW`), boot, then run `examples/hot-reload-demo/smoke.sh` -- `=== PASS: post-recompile response contains expected marker ===`; this is the HTTP half of the example's dual verification, alongside its headless sentinel profile at step 2.
12. Deploy http-app (`cp -R examples/http-app src/usr/WWW`), boot, then run the three curl probes from `examples/http-app/README.md` Verify -- `ok`, the echoed body, and `404 Not Found` respectively. This example has no bundled `smoke.sh` in this tree; the probes are manual.
13. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/https-smoke.sh` -- `HTTPS-SMOKE PASS` after the nine native-TLS phases (certless stand-down, `tls-cert reload` activation, five service probes, and the two statedump key-scans). Needs the lpc-ext crypto module built (`make crypto` in `dworkin/lpc-ext`); without it this step is the documented skip -- the platform's TLS posture degrades cleanly and the other steps do not exercise it.

This is the pre-PR bar `CONTRIBUTING.md`'s Testing section points to.

## Adding a regression

| Change class | Regression surface | Mechanic |
|---|---|---|
| Console / operator-visible behavior | A verbset block, added to an existing file or a new one under `scripts/verbsets/` | Add a `cmd:` line plus `expect:`/`absent:` regex lines per the block format in the `drive-verbs.py + scripts/verbsets/` section above; wire a new file into `drive-verbs-smoke.sh`'s default verbset list (or drive it explicitly) so the sweep runs it. |
| Boot-time or primitive behavior | A sentinel phase in the example's `sys/test.c`, plus the profile's expected-OK bump in `run-example.sh` | Add a phase that appends an " OK" (or a `FAIL`) line to the deployed domain's `data/test-result.log`, then bump the matching `ok` field in `example_profile()`. The script's own header names this mechanic directly: "bump when a test-driver phase adds a sentinel". |
| Boot hygiene / noise | `base-boot-guard.sh`'s line bound (`MAX_LINES`, default 400) | A change that adds legitimate boot-log or `system.log` output raises the guard's baseline; if the new output pushes past the bound, raise `MAX_LINES` deliberately with a stated reason rather than silencing the guard -- an unexplained jump toward the bound is the atomic-write-storm regression this guard exists to catch. |

## Where to next

- `docs/application-authoring.md` Testing your application -- the sentinel-driver pattern from the application-author side.
- `CONTRIBUTING.md` Testing -- what test evidence a change needs.
- `docs/runtime-primitives.md` -- the per-primitive Demonstration entries these scripts back.
