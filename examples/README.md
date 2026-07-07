# Examples

Runnable evidence: each directory below is a minimal, deployable application demonstrating the platform's runtime primitives against a live DGD boot, not a diagram or a claim in prose.

`DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh <example>` is the one-command harness -- clean-slate deploy, boot cycle, sentinel-count assertion -- for seven of the nine examples (see the profile table at the top of the script for which). The other two, `atomic-demo` and `http-app`, verify against a running server via HTTP probes; `hot-reload-demo` verifies both ways. `../docs/README.md`'s Working examples section is the authoritative map from each example to its companion doc and reading path.

- `http-app/` -- HTTP/1 contracts (`GET /health`, `POST /echo`, 404 fallback). Doc: `../docs/http-applications.md`. Verify: manual `curl` steps in the README.
- `vault-app/` -- schema-driven typed-property marshaling and on-disk XML round-trip. Doc: `../docs/vault-applications.md`. Verify: `scripts/run-example.sh vault-app` (sentinel log).
- `signal-app/` -- the smallest signal-on-property demonstration: one observer, one write, one synchronous-fire assertion. Doc: `../docs/signal-applications.md`. Verify: `scripts/run-example.sh signal-app` (sentinel log).
- `merry-app/` -- Merry's invocation surface: ancestry walk, sandbox, batching, dispatcher, observer lifecycle, persistence across snapshot restore. Doc: `../docs/merry-applications.md`. Verify: `scripts/run-example.sh merry-app` (sentinel log).
- `chat-app/` -- multi-user chat spanning capability gates, sandboxed reactions, atomic events, coherent concurrent writes, and a three-boot persistence cycle. Doc: `../docs/chat-applications.md`. Verify: `scripts/run-example.sh chat-app` (sentinel log).
- `atomic-demo/` -- an `atomic` function errors mid-mutation; the runtime rolls the mutation back. Doc: `../docs/runtime-primitives.md` §1. Verify: `./smoke.sh` (HTTP probe).
- `hot-reload-demo/` -- `compile_object` recompiles a live target; the next dispatch runs the new program, no restart. Doc: `../docs/runtime-primitives.md` §4. Verify: `./smoke.sh` (HTTP probe) or `scripts/run-example.sh hot-reload-demo` (headless sentinel profile).
- `hot-reload-master/` -- recompiling a clonable master propagates the new program to existing clones while each keeps its own state. Doc: `../docs/code-lifecycle.md`. Verify: `scripts/run-example.sh hot-reload-master` (sentinel log).
- `upgrade-cascade/` -- upgrading a parent library through the upgrade daemon recompiles its inheritors and `call_touch`-patches their existing clones, state intact. Doc: `../docs/code-lifecycle.md` Library upgrade. Verify: `scripts/run-example.sh upgrade-cascade` (sentinel log).

## Where to next

- [`../docs/README.md`](../docs/README.md) Working examples -- the authoritative example-to-doc map and reading paths.
- [`../docs/getting-started.md`](../docs/getting-started.md) -- first-time install and the bundled example configuration.
- [`../docs/runtime-primitives.md`](../docs/runtime-primitives.md) -- the runtime primitives these examples demonstrate (seven of the eight; state introspection is verified interactively at the console).
