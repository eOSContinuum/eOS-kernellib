# Regression harness

**Audience**: a contributor running or extending the headless boot regressions under `scripts/`; assumes DGD is built per `docs/getting-started.md`.

Ten scripts back the regression surface. The six shell scripts drive the platform through boot cycles and assert the result; they resolve `DGD_BIN` (env override, falling back to `dgd` on `PATH`) and refuse to start if a `dgd` instance already holds the ports. `drive-verbs.py` launches nothing — it is the telnet client the others use against an already-running instance; `measure-baseline.py` is the timing rig; the two `gen-*.py` vector generators write test fixtures and never launch the platform.

## run-example.sh

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh <example>
EXPECTED_OK=21 scripts/run-example.sh chat-app   # override the profile's count
```

Deploys the named example fresh under `src/usr/<Deploy>`, runs its boot sequence, then counts `" OK"` lines in the deployed domain's `data/test-result.log` against the expected count; a `FAIL` line or a count mismatch is a non-zero exit. A `selfexit` boot 1 waits (30s cap) for the driver to dump-and-exit on its own; a `timed` boot 1 runs a fixed 6s window then is killed. `Boots >= 2` restarts against `state/snapshot` (the restore boot); `Boots >= 3` adds a third cold boot with no snapshot loaded (the cold-boot negative case). Each example's recipe is one line in the script's `example_profile()` function.

| Example | Deploy | Boots | Boot 1 | Expected OK |
|---|---|---|---|---|
| agent-app | AgentApp | 1 | timed | 10 |
| atomic-demo | WWW | 1 | timed | 3 |
| chat-app | Chat | 3 | selfexit | 20 |
| composite-app | WWW+Inventory | 2 | selfexit | 5 |
| hot-reload-demo | WWW | 1 | timed | 2 |
| hot-reload-master | Reload | 1 | timed | 3 |
| merry-app | MerryApp | 2 | selfexit | 28 |
| signal-app | SignalApp | 1 | timed | 1 |
| upgrade-cascade | Cascade | 1 | timed | 7 |
| vault-app | MyApp | 1 | timed | 10 |
| webauthn-app | WebAuthn | 1 | selfexit | 13 |

A `+`-joined Deploy is a multi-domain example: one example subdirectory per part, each deployed as its own domain, with sentinels read from the last part's `data/`. Module notes: `agent-app` needs `LPC_EXT_CRYPTO` for every phase, and its operator continuation (4 more sentinels) is driven by `scripts/verbsets/agent-app.verbset` -- see the example README. `composite-app`'s 5 is the module-less transport subset; the full set is `LPC_EXT_CRYPTO=<module> EXPECTED_OK=38`. `webauthn-app`'s 13 is the codec half; the ceremony phases need the module (`EXPECTED_OK=26`).

`http-app` has no profile here -- it verifies over live HTTP via the probe commands in its README; `https-app` verifies via `scripts/https-smoke.sh`; `atomic-demo` and `hot-reload-demo` verify both ways (their profiles above, plus each example's bundled HTTP smoke).

## drive-verbs.py + scripts/verbsets/

```sh
python3 scripts/drive-verbs.py scripts/verbsets/admin-baseline.verbset
```

Logs into the console over telnet, then drives each block of a verbset file in session order. A verbset block is one `cmd:` line (the verb to send) plus zero or more `expect:` / `absent:` regex lines (`re.search`, `re.MULTILINE`) checked against the response; blocks separate on a blank line, `#` lines are comments. An `expect:` that fails to match, or an `absent:` that matches, both fail the entry; the run exits non-zero on any entry failure. Consecutive entries share one session, so a verbset can express mutate -> verify -> undo -> verify-undo. A block may also carry `capture: <name> <regex-with-one-group>` lines: on match, the group's text is stored and later `cmd:`/`expect:`/`absent:`/`capture:` lines can reference it as `%{name}` (regex-escaped inside patterns, raw in cmd lines) -- the mechanism for threading runtime values (a minted id, a generated secret shown once) through later verbs; a capture that does not match fails its block, and later references to it fail theirs.

Two composability constraints when batching verbsets into one boot: a verbset asserting a daemon's module-less stand-down cannot share a module-bearing boot (the stand-down refusals it expects answer differently with the crypto module loaded), and a verbset asserting absolute counts (identity rosters, store totals, patch counters) needs its own boot -- state accumulated by earlier verbsets in the same session shifts the counts.

The session logs in as `admin` unless the verbset declares otherwise: file-level `user:` / `password:` directives before the first block (overridable with `--user` / `--password`) let a verbset run as a registered non-admin operator -- the login shape that reaches the System console and its lifecycle verbs (`upgrade` and friends; see `docs/admin-console.md` Connecting). Cold boots register no such operator, so a registered-user verbset runs after a provisioning verbset driven as admin: `operator-provision.verbset` grants the operator, `operator-upgrade.verbset` (`user: testop`) then drives the console `upgrade -p` cascade against a deployed upgrade-cascade example.

## drive-verbs-smoke.sh

```sh
scripts/drive-verbs-smoke.sh                  # default: all ten verbsets over vault-app + upgrade-cascade deploys
DEPLOY="<example>:<Mount> ..." scripts/drive-verbs-smoke.sh <verbset ...>
```

Boots the platform headless and drives verbset file(s) with `drive-verbs.py` against the live telnet console, then shuts down. The default run covers admin-baseline, logging-verbs, schema-verbs, dispatcher-verbs, port-labels, tls-cert, identity-verbs, session-verbs, operator-provision, and operator-upgrade in one boot, deploying vault-app as the `MyApp` domain and upgrade-cascade as the `Cascade` domain first -- the dispatcher-verbs clone-addressing cycle drives the property-bearing named clone (`MyApp:core:item1`) the vault-app boot driver creates, the operator cycle drives `upgrade -p` against the settled cascade deploy, and neither example self-exits. The clean-slate step also removes the provisioned operator's artifacts (`src/usr/testop/`, the kernel's persisted access list) so every run reasserts the cold-boot registration flow. With explicit verbset arguments nothing is deployed unless `DEPLOY` asks for it; `DEPLOY` -- a space-separated list of `<example>:<Mount>` pairs -- also overrides the default run's deployment. A boot here is always cold, so a `selfexit` example (one whose driver calls `shutdown()` when it finishes) tears the console down before verbs can run -- drive those against a non-selfexit deployment, or rely on the example's own in-application test phases instead.

## https-smoke.sh

```sh
LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
DGD_BIN=/path/to/dgd/bin/dgd scripts/https-smoke.sh
```

Native-TLS end-to-end, covering the binding and its certificate surface in nine phases. Deploys `examples/https-app` as the `WWW` domain and boots with a second binary port and the lpc-ext crypto module loaded but no certificate (the `LPC_EXT_CRYPTO` path is appended as a `modules` line to the generated config -- the checked-in `example.dgd` stays module-less). Phase 1 proves the bootstrap stood down honestly (`tls-cert` status, no HTTPS service); phase 2 generates a throwaway self-signed P-256 certificate under `src/usr/System/data/tls/` (removed after the run) and activates it with `tls-cert reload` -- registration without a restart; phases 3-7 drive the service probes (`GET /health`, negotiated TLS 1.3, `POST /echo` round-trip, 404 route, cleartext refusal); phases 8-9 take a console `snapshot` twice -- idle and with a live established TLS connection held open -- and scan the statedump for the private key in every in-memory representation (DER, PEM base64, raw scalar), with the port registry's `https` label as the scan's positive control. HTTP probes use `openssl s_client`, not curl -- the stock macOS curl's SecureTransport backend cannot speak TLS 1.3; console phases ride `drive-verbs.py` with an ephemeral verbset under `state/`. `HTTPS-SMOKE PASS` is the pass signal.

## session-smoke.sh

```sh
LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
DGD_BIN=/path/to/dgd/bin/dgd scripts/session-smoke.sh
```

The session daemon end-to-end with its statedump-discipline proof. Boots with the crypto module and drives the session lifecycle over the console -- mint (capturing the plaintext token from the transcript), validate, revoke, TTL expiry, revoke-principal. The load-bearing phase takes a console `snapshot` while a session is live and scans the image for the plaintext token: it must be absent, while the token's SHA-256 hash and the principal string are present (their absence would mean the session never persisted and the scan is vacuous -- the positive control). `SESSION-SMOKE PASS` is the pass signal. Needs the lpc-ext crypto module (`make crypto` in `dworkin/lpc-ext`); the daemon's module-less stand-down is asserted by session-verbs in the drive-verbs default suite.

## agent-smoke.sh

```sh
LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
DGD_BIN=/path/to/dgd/bin/dgd scripts/agent-smoke.sh
```

Agent identity end-to-end with its statedump-discipline proof. Boots with the crypto module and drives, over the console: the key ceremony against a real foreign signer (Ed25519 via the `openssl` CLI, so the platform verifies bytes it did not produce), including the domain-tag refusal (a signature over the bare challenge is refused); the token ceremony including required expiry (a short-TTL token authenticates, then expires and is refused); suspension killing live sessions and delegated grants while a coexisting operator grant of the same capability survives on its own source, with resume restoring authentication but never grants; and the load-bearing scan -- a statedump taken with a live agent token and a live agent session is scanned for the token plaintext (must be absent) with the token's SHA-256 hash and the agent principal as positive controls. `AGENT-SMOKE PASS` is the pass signal. Needs python3 (stdlib only) and an `openssl` CLI with Ed25519 support (OpenSSL 1.1.1+; LibreSSL will not do), plus the crypto module like the other module-bearing steps.

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

## Vector generators

```sh
python3 scripts/gen-webauthn-vectors.py
python3 scripts/gen-agent-vectors.py
```

Both write generated fixtures; do not edit those by hand -- rerun the generator. `gen-webauthn-vectors.py` produces `examples/webauthn-app/sys/vectors.h`, `scripts/verbsets/webauthn-ceremony.verbset`, and `scripts/verbsets/identity-recovery.verbset`; `gen-agent-vectors.py` produces `examples/agent-app/sys/vectors.h`. The signatures come from independent implementations -- the python `cryptography` package for the WebAuthn ECDSA vectors, the `openssl` CLI for the agent Ed25519 vectors -- so the platform's verify kfuns and ceremony parsing are checked against foreign-generated payloads, not bytes the LPC stack produced itself. Regenerated files differ byte-for-byte (fresh keys, randomized ECDSA) while staying valid; the structures the LPC drivers assert against (rpId, origin, challenges, credential ids, the domain tag) are fixed in the generators. `gen-webauthn-vectors.py` needs python3 with the `cryptography` package; `gen-agent-vectors.py` needs python3 (stdlib only) and an `openssl` CLI with Ed25519 support (OpenSSL 1.1.1+; LibreSSL will not do).

## Full regression sweep

Run in this order for the complete pre-PR bar. Each line names the command and the pass signal to look for; `<dgd>` is the path to a built DGD binary. Expect the full bar to take on the order of fifteen minutes end to end on the measured-baseline hardware -- no single step exceeds about two minutes.

1. `DGD_BIN=<dgd> scripts/run-example.sh chat-app` -- `PASS`, 20 " OK" sentinels across 3 boots (cold selfexit, snapshot restore, cold no-snapshot).
2. `DGD_BIN=<dgd> scripts/run-example.sh hot-reload-demo` -- `PASS`, 2 " OK" sentinels (1 timed boot).
3. `DGD_BIN=<dgd> scripts/run-example.sh hot-reload-master` -- `PASS`, 3 " OK" sentinels (1 timed boot).
4. `DGD_BIN=<dgd> scripts/run-example.sh merry-app` -- `PASS`, 28 " OK" sentinels across 2 boots (cold selfexit, snapshot restore).
5. `DGD_BIN=<dgd> scripts/run-example.sh signal-app` -- `PASS`, 1 " OK" sentinel (1 timed boot).
6. `DGD_BIN=<dgd> scripts/run-example.sh upgrade-cascade` -- `PASS`, 7 " OK" sentinels (1 timed boot).
7. `DGD_BIN=<dgd> scripts/run-example.sh vault-app` -- `PASS`, 10 " OK" sentinels (1 timed boot).
8. `DGD_BIN=<dgd> scripts/run-example.sh webauthn-app` -- `PASS`, 13 " OK" sentinels (1 cold selfexit boot; the codec phases -- ceremony phases skip without the crypto module).
9. `LPC_EXT_CRYPTO=<crypto-module> EXPECTED_OK=26 DGD_BIN=<dgd> scripts/run-example.sh webauthn-app` -- `PASS`, 26 " OK" sentinels: the codec phases plus the 13 ceremony phases (registration and assertion against foreign-generated vectors, with the negative batteries: bad type, bad challenge, bad origin, bad rpIdHash, bad format, bad signature, wrong key, and the Ed25519 pair). Without the module this step is the documented skip.
10. `DGD_BIN=<dgd> scripts/run-example.sh composite-app` -- `PASS`, 5 " OK" sentinels across 2 boots (cold selfexit, snapshot restore): the module-less transport subset.
11. `LPC_EXT_CRYPTO=<crypto-module> EXPECTED_OK=38 DGD_BIN=<dgd> scripts/run-example.sh composite-app` -- `PASS`, 38 sentinels: the full composite set (WebAuthn/session authentication over the wire, the agent self-service panel, the server-sent-event streams, the recovery ceremony, and the restore boot). Without the module the subset at step 10 is the documented fallback.
12. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/run-example.sh agent-app` -- `PASS`, 10 sentinels: controller registration through authd, self-service agent minting, the foreign-signed key ceremony with its refusals, and suspension/resume. Every phase needs the module; without it this step is the documented skip.
13. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> DEPLOY="agent-app:AgentApp" scripts/drive-verbs-smoke.sh scripts/verbsets/agent-app.verbset` -- `DRIVE-VERBS PASS` after the operator continuation of step 12's example: the capability grant and delegable flip, then the driver's continuation sentinels (the delegation lands, the action gate opens, suspension kills the delegated grant, resume restores nothing).
14. `DGD_BIN=<dgd> scripts/run-example.sh atomic-demo` -- `PASS`, 3 " OK" sentinels (1 timed boot): the headless half of the atomicity demonstration -- the pre-call baseline, the caught deliberate failure with the counter's own error text as the body-ran control, and the post-catch rollback assertion. The HTTP half is step 25.
15. `DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh` -- `DRIVE-VERBS PASS` after the ten default verbsets (admin-baseline, logging-verbs, schema-verbs, dispatcher-verbs, port-labels, tls-cert, identity-verbs, session-verbs, operator-provision, operator-upgrade) run against the vault-app (MyApp) and upgrade-cascade (Cascade) deploys.
16. `DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/code-eval-baseline.verbset scripts/verbsets/initd-laundering.verbset scripts/verbsets/kvstore-roundtrip.verbset scripts/verbsets/agent-records.verbset` -- `DRIVE-VERBS PASS` after the four module-less console regressions in one base boot: the code verb's float-classification and fault-reporting baseline, the removal of the initd port-manager laundering publics, the KVstore/BTree round-trip through node splits and merges, and the substrate-tier gating of the identityd agent operations.
17. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/identity-lifecycle.verbset` -- `DRIVE-VERBS PASS` after the active identity lifecycle (operator mint with recovery codes, single-use redemption, the never-zero-credentials invariant, atomic rotation rollback, revocation). Needs the lpc-ext crypto module like the https-smoke step; without it this step is the documented skip -- identityd's module-less stand-down is asserted by identity-verbs in step 15.
18. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/identity-capability.verbset` -- `DRIVE-VERBS PASS` after the operator grant path: minting an identity, granting a platform capability to its principal, confirming `capabilityd->is_allowed` answers true through the choke-point, the KERNEL()-gated `grant` refusing a console caller, and revocation. Needs the lpc-ext crypto module (minting); without it the skip is documented.
19. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/webauthn-ceremony.verbset` -- `DRIVE-VERBS PASS` after the ceremony daemon's console drive: TOFU registration minting an identity, re-registration refused, assertion advancing signCount, the replay refusal, an unknown credential refused, and the System-tier gate. Regenerate the vectors (and this verbset) with `scripts/gen-webauthn-vectors.py`; without the module this step is the documented skip.
20. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/identity-recovery.verbset` -- `DRIVE-VERBS PASS` after the operator half of the recovery doctrine: mint-with-codes, the console `identity bind` verb verifying a foreign attestation before binding, the never-bare-re-bind refusal, single-use redemption, and the last-credential redemption refusal. Regenerated by the same vector script as step 19; without the module this step is the documented skip.
21. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/drive-verbs-smoke.sh scripts/verbsets/agent-auth.verbset scripts/verbsets/agent-console.verbset scripts/verbsets/agent-delegation.verbset` -- `DRIVE-VERBS PASS` after the agent substrate's three console suites in one module-bearing boot: the authd agent entry points (self-service minting, the token ceremony, structural non-transitivity, suspension semantics), the operator verb face (mint, show extensions, suspend/resume, operator-driven delegation), and the delegation registry with its source-tracked revocation cascade. Without the module these are the documented skip; the module-less gates are covered by agent-records in step 16.
22. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/session-smoke.sh` -- `SESSION-SMOKE PASS` after the session lifecycle (mint, validate, revoke, TTL expiry, revoke-principal) and the statedump-discipline scan: the plaintext token is absent from a live-session snapshot while its hash and principal are present. Needs the lpc-ext crypto module; the module-less stand-down is asserted by session-verbs in step 15.
23. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/agent-smoke.sh` -- `AGENT-SMOKE PASS` after the foreign-signer key ceremony with the domain-tag refusal, the token ceremony with required expiry, the suspension and coexisting-grant semantics, and the statedump token-plaintext scan with its positive controls. Needs python3 and an OpenSSL 1.1.1+ CLI besides the module.
24. `DGD_BIN=<dgd> scripts/base-boot-guard.sh` -- `GUARD PASS`, boot log and `system.log` both at or under 400 lines (`MAX_LINES`, no example deployed).
25. Deploy atomic-demo (`cp -R examples/atomic-demo src/usr/WWW`), boot against `example.dgd`, then run `examples/atomic-demo/smoke.sh` -- `=== PASS: counter unchanged across deliberate-failure increment ===`; the HTTP half of the example's dual verification, alongside its headless sentinel profile at step 14. Steps 25-27 are manual deploys with no built-in clean slate: before each, stop the running `dgd` and remove the prior deploy and state (`rm -rf src/usr/WWW state/snapshot state/snapshot.old state/swap`) -- `cp -R` into an existing mount nests the new example inside the old one instead of replacing it, silently deploying a broken tree.
26. Same reset first, then deploy hot-reload-demo (`cp -R examples/hot-reload-demo src/usr/WWW`), boot, then run `examples/hot-reload-demo/smoke.sh` -- `=== PASS: post-recompile response contains expected marker ===`; this is the HTTP half of the example's dual verification, alongside its headless sentinel profile at step 2.
27. Same reset first, then deploy http-app (`cp -R examples/http-app src/usr/WWW`), boot, then run the four curl probes from `examples/http-app/README.md` Verify -- `ok`, the `/status` count lines, the echoed body, and `404 Not Found` respectively. This example has no bundled `smoke.sh` in this tree; the probes are manual.
28. `LPC_EXT_CRYPTO=<crypto-module> DGD_BIN=<dgd> scripts/https-smoke.sh` -- `HTTPS-SMOKE PASS` after the nine native-TLS phases (certless stand-down, `tls-cert reload` activation, five service probes, and the two statedump key-scans). Needs the lpc-ext crypto module built (`make crypto` in `dworkin/lpc-ext`); without it this step is the documented skip -- the platform's TLS posture degrades cleanly and the other steps do not exercise it.

This is the pre-PR bar `CONTRIBUTING.md`'s Testing section points to.

## Adding a new example

A whole new example registers at six points; missing one either breaks reruns or leaves the example undiscoverable:

1. **The directory shape**: `examples/<name>/` with an `initd.c` (the domain entry point the System initd's `/usr/[A-Z]*/initd.c` iteration picks up) and a `sys/test.c` boot-time driver appending ` OK` / `FAIL` sentinels to the deployed domain's `data/test-result.log`.
2. **`run-example.sh`, twice**: a profile line in `example_profile()` (deploy mount, boots, boot-1 mode, expected OK count) and the mount name added to the clean-slate removal loop -- a mount missing from that loop survives into other examples' runs and breaks their isolation.
3. **The profile table** in this README, mirroring the new `example_profile()` line.
4. **`examples/README.md`**: the index row (description, companion doc, verify path).
5. **`docs/README.md` Working examples**: the map row.
6. **`.gitignore`**: the deploy mount (`src/usr/<Mount>/`) -- deploy targets are individually ignored, and an unlisted mount shows up as untracked noise after every run.

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
