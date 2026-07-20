# Examples

Runnable evidence: each directory below is a minimal, deployable application demonstrating the platform's runtime primitives against a live DGD boot, not a diagram or a claim in prose.

`DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh <example>` is the one-command harness -- clean-slate deploy, boot cycle, sentinel-count assertion -- for eleven of the thirteen examples (see the profile table at the top of the script for which). `http-app` verifies against a running server via HTTP probes, `https-app` via `scripts/https-smoke.sh`; `atomic-demo` and `hot-reload-demo` verify both ways. `../docs/README.md`'s Working examples section is the authoritative description and reading-path map; the rows below are one-liners, and each example's own README plus the harness profile carry the mutable sentinel counts (the verify command surfaces them).

- `http-app/` -- HTTP/1 contracts. Doc: `../docs/http-applications.md`. Verify: manual `curl` steps in the README.
- `https-app/` -- the native-TLS mount point. Doc: `../docs/operations.md` Network boundary and transport security. Verify: `scripts/https-smoke.sh` (needs `LPC_EXT_CRYPTO`).
- `vault-app/` -- schema-driven typed-property marshaling and on-disk XML round-trip. Doc: `../docs/vault-applications.md`. Verify: `scripts/run-example.sh vault-app`.
- `signal-app/` -- the smallest signal-on-property demonstration. Doc: `../docs/signal-applications.md`. Verify: `scripts/run-example.sh signal-app`.
- `merry-app/` -- Merry's invocation surface. Doc: `../docs/merry-applications.md`. Verify: `scripts/run-example.sh merry-app`.
- `chat-app/` -- multi-user chat across capability gates, sandboxed reactions, atomic events, coherent concurrent writes, and a three-boot persistence cycle. Doc: `../docs/chat-applications.md`. Verify: `scripts/run-example.sh chat-app`.
- `composite-app/` -- the composite transport-connected application (two domains, wire auth, a persistent daemon with an audit observer, the agent surface, event streams, recovery, a restore boot). Doc: `../docs/composite-applications.md`. Verify: `scripts/run-example.sh composite-app` (with `LPC_EXT_CRYPTO` + `EXPECTED_OK` for the full set).
- `atomic-demo/` -- an `atomic` function errors mid-mutation; the runtime rolls it back. Doc: `../docs/runtime-primitives.md` §1. Verify: `scripts/run-example.sh atomic-demo` or `./smoke.sh`.
- `hot-reload-demo/` -- `compile_object` recompiles a live target; the next dispatch runs the new program. Doc: `../docs/runtime-primitives.md` §4. Verify: `./smoke.sh` or `scripts/run-example.sh hot-reload-demo`.
- `hot-reload-master/` -- recompiling a clonable master propagates to existing clones, each keeping its state. Doc: `../docs/code-lifecycle.md`. Verify: `scripts/run-example.sh hot-reload-master`.
- `upgrade-cascade/` -- an upgrade daemon recompiles a library's inheritors and `call_touch`-patches their clones, state intact. Doc: `../docs/code-lifecycle.md` Library upgrade. Verify: `scripts/run-example.sh upgrade-cascade`.
- `webauthn-app/` -- the WebAuthn codec and ceremony substrate. Doc: `../docs/kernel-libraries.md` Utilities. Verify: `scripts/run-example.sh webauthn-app` (full set needs `LPC_EXT_CRYPTO`).
- `agent-app/` -- the agent-identity worked example. Doc: `../docs/identity.md` Agent identities. Verify: `scripts/run-example.sh agent-app` (needs `LPC_EXT_CRYPTO`; the operator continuation runs via `scripts/verbsets/agent-app.verbset`).

## Where to next

- [`../docs/README.md`](../docs/README.md) Working examples -- the authoritative example-to-doc map and reading paths.
- [`../docs/getting-started.md`](../docs/getting-started.md) -- first-time install and the bundled example configuration.
- [`../docs/runtime-primitives.md`](../docs/runtime-primitives.md) -- the runtime primitives these examples demonstrate (seven of the eight; state introspection is verified interactively at the console).
