# Regression harness

**Audience**: a contributor running or extending the headless boot regressions under `scripts/`; assumes DGD is built per `docs/getting-started.md`.

Four scripts drive the platform through boot cycles and assert on the result. The three shell scripts resolve `DGD_BIN` (env override, falling back to `dgd` on `PATH`) and refuse to start if a `dgd` instance already holds the ports; `drive-verbs.py` launches nothing — it is the telnet client the others use against an already-running instance.

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

`atomic-demo` and `http-app` have no profile here -- they verify over live HTTP (see each README and bundled `smoke.sh`).

## drive-verbs.py + scripts/verbsets/

```sh
python3 scripts/drive-verbs.py scripts/verbsets/admin-baseline.verbset
```

Logs into the console over telnet, then drives each block of a verbset file in session order. A verbset block is one `cmd:` line (the verb to send) plus zero or more `expect:` / `absent:` regex lines (`re.search`, `re.MULTILINE`) checked against the response; blocks separate on a blank line, `#` lines are comments. An `expect:` that fails to match, or an `absent:` that matches, both fail the entry; the run exits non-zero on any entry failure. Consecutive entries share one session, so a verbset can express mutate -> verify -> undo -> verify-undo.

The session logs in as `admin` unless the verbset declares otherwise: file-level `user:` / `password:` directives before the first block (overridable with `--user` / `--password`) let a verbset run as a registered non-admin operator -- the login shape that reaches the System console and its lifecycle verbs (`upgrade` and friends; see `docs/admin-console.md` Connecting). Cold boots register no such operator, so a registered-user verbset runs after a provisioning verbset driven as admin: `operator-provision.verbset` grants the operator, `operator-upgrade.verbset` (`user: testop`) then drives the console `upgrade -p` cascade against a deployed upgrade-cascade example.

## drive-verbs-smoke.sh

```sh
scripts/drive-verbs-smoke.sh                  # default: all six verbsets over vault-app + upgrade-cascade deploys
DEPLOY="<example>:<Mount> ..." scripts/drive-verbs-smoke.sh <verbset ...>
```

Boots the platform headless and drives verbset file(s) with `drive-verbs.py` against the live telnet console, then shuts down. The default run covers admin-baseline, logging-verbs, schema-verbs, dispatcher-verbs, operator-provision, and operator-upgrade in one boot, deploying vault-app as the `MyApp` domain and upgrade-cascade as the `Cascade` domain first -- the dispatcher-verbs clone-addressing cycle drives the property-bearing named clone (`MyApp:core:item1`) the vault-app boot driver creates, the operator cycle drives `upgrade -p` against the settled cascade deploy, and neither example self-exits. The clean-slate step also removes the provisioned operator's artifacts (`src/usr/testop/`, the kernel's persisted access list) so every run reasserts the cold-boot registration flow. With explicit verbset arguments nothing is deployed unless `DEPLOY` asks for it; `DEPLOY` -- a space-separated list of `<example>:<Mount>` pairs -- also overrides the default run's deployment. A boot here is always cold, so a `selfexit` example (one whose driver calls `shutdown()` when it finishes) tears the console down before verbs can run -- drive those against a non-selfexit deployment, or rely on the example's own in-application test phases instead.

## base-boot-guard.sh

```sh
MAX_LINES=400 scripts/base-boot-guard.sh   # override the bound (default 400)
```

Boots the platform with no example deployed for a fixed 6s window, then asserts both the boot log and `src/usr/System/log/system.log` stay at or under `MAX_LINES` -- a regression against the atomic-write-storm failure mode (a caught error inside an atomic function re-entering the error manager and looping a file write).

## Where to next

- `docs/application-authoring.md` Testing your application -- the sentinel-driver pattern from the application-author side.
- `CONTRIBUTING.md` Testing -- what test evidence a change needs.
- `docs/runtime-primitives.md` -- the per-primitive Demonstration entries these scripts back.
