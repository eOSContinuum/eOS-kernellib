# Common tasks

Task-shaped recipes for the application author's recurring jobs after `docs/first-application.md`: each names a goal, the steps, a verification, and the document that owns the mechanism. Nothing here introduces new doctrine -- these are the how-to shapes of what the explanation docs already state, plus the port-registration mechanics recovered from source.

**Audience**: an application author who has a domain running (per `docs/first-application.md` or `docs/application-authoring.md`) and needs the short version of a recurring task, with the owning doc one link away.

## Add a boot-time test driver to your domain

**Goal**: your domain's regression runs at cold boot and writes sentinel lines an external script can assert on.

1. Add `sys/test.c` to your domain. In `create()`, defer the run with `call_out("setup_and_run", 0)` (the name every shipped driver uses) so every domain's initd has finished before the driver calls cross-domain daemons.
2. Wrap each test phase in its own `catch {}` and append one line per phase to `/usr/<App>/data/test-result.log`: `"<App>:test: <PHASE> OK"` on success, `"<App>:test: FAIL: <reason>"` on failure. Name sentinels by what they assert. The shared `log_line` helper and the annotated reference driver (`examples/chat-app/sys/test.c`) are in `docs/application-authoring.md` The sentinel-driver pattern.
3. Compile the driver from your initd like any other daemon.

**Verify**: cold-boot the platform and read the result log; wire it into `scripts/run-example.sh`'s counting (an `" OK"` count plus a FAIL grep) for CI.

**Owning doc**: `docs/application-authoring.md` Testing your application.

## Schedule recurring or oversized work

**Goal**: work that exceeds one tick budget, or must recur, runs in slices without blocking the task queue.

1. Process a bounded chunk per call, save the cursor in object state, and re-arm with `call_out("continue_work", 0, cursor)`. Each fired call_out runs under a fresh tick budget, and each completed slice's mutations stand on their own -- a handler is an ordinary non-atomic call unless declared `atomic`, so declare it `atomic` if a mid-slice error must roll the slice back, and design chunk boundaries so every completed slice leaves consistent state (worked example: `docs/application-authoring.md` Spreading work across timeslices).
2. For recurring work on a period, re-arm with the period as the delay instead of `0`; keep the re-arm call at the end of the handler so a thrown error skips it rather than looping a failure.
3. For sequenced multi-step work, build a `Continuation` chain instead of hand-rolled call_out bookkeeping: `new Continuation("step1")` then `chain("step2")` receives step1's return value, then one `runNext()` starts the chain. The family (iterative, delayed, distributing) is catalogued in `docs/kernel-libraries.md` Asynchronous control.

**Verify**: watch the slices land (`status` shows call_outs pending; your object's cursor advances between slices).

**Owning doc**: `docs/application-authoring.md`; `docs/execution-model.md` for why each slice gets a fresh budget.

## Migrate live state after a data-shape change

**Goal**: existing clones (including ones restored from old snapshots) adopt a new field layout when their master's program changes.

1. Give the clonable a `patch()` hook that is idempotent: check a format-version property, transform old fields to new, stamp the new version. `patch()` takes no arguments and runs inside an atomic context before the intercepted call proceeds.
2. Recompile the changed sources through the upgrade cascade with patching queued: from the System login console, `upgrade -p <file.c>`.
3. The upgrade daemon marks every live clone with `call_touch` and then drives the patch sweep itself, one zero-delay callout per object -- an eager sweep, not a wait-for-next-reference.
4. If the domain persists through the Vault, the sweep above covered live clones only: stored XML under the Vault's data tree is untouched by `patch()`, and old-shape files meet the new schema at their next respawn (removed scalar fields dropped silently, a removed `lpc_obj` field able to fail the whole configure, added fields defaulted, renamed types skipping the import entirely). Run the respawn-and-re-store sweep in `docs/vault-applications.md` Schema evolution for the on-disk half, and grep `system.log` for `Warning:: Schema node` and `VAULT: Configuration failed` afterward.

**Verify**: `issues <file.c>` reads back whether the cascade fully propagated; probe a pre-existing clone's format-version property with `code`.

**Owning doc**: `docs/code-lifecycle.md` Touch; `docs/changing-a-running-system.md` rung 3; `docs/vault-applications.md` Schema evolution for the on-disk half.

## Grant another domain access to your files

**Goal**: domain `Foo` can read (or write) under `/usr/Bar/`.

1. Operationally, from the admin console: `grant Foo /usr/Bar/lib read` (`write` is the default when the mode keyword is omitted; `full` also exists). `ungrant` reverses it, and `access <user>` / `access <directory>` audit the current bits.
2. `grant global <directory>` instead adds a `/usr/`-subdirectory to the global-read set -- the right shape when every domain should read a shared library.
3. At boot time, grants are System-tier acts: the access API (`set_access`) is reachable from System code, not from a tier-E domain's own initd -- a domain cannot grant itself access to anything. Route boot-time grant needs through your platform overlay's System-tier initialization, or provision them once from the console (grants persist in the kernel's saved access data).

**Verify**: `access Foo` lists the grant; a `read_file` from Foo's code stops erroring.

**Owning doc**: `docs/admin-console.md` (Permissions verbs); `docs/application-authoring.md` Owner and access.

## Add an operator verb for your application

**Goal**: an operator at the console can run `myapp-status` instead of a `code` one-liner.

1. Know the honest constraint first: the console's extension-verb table is a hardcoded mapping in the kernel registry (`src/kernel/sys/admin_console_registry.c` `create()`), and there is no dynamic registration surface. Adding a verb is a platform contribution -- an edit to that table -- not an application-local act.
2. Supply an extension object in your domain exposing a `cmd_<verb>(...)` method per registered entry, and add the verb-to-path/method row to the registry's `dispatch_table`. The registry seeds the `admin_console.caller` capability that gates who may invoke extension handlers; the Merry console extension (`src/usr/Merry/lib/admin_console_ext.c` and its registry rows) is the worked example.
3. Until the platform grows a registration surface, the application-local alternative is a documented `code` call on your own daemon (`code "/usr/MyApp/sys/myappd"->status()`), which needs no kernel edit.

**Verify**: the new verb answers on the kernel console (`admin` login); `No command` means the row or the extension object path is wrong.

**Owning doc**: `docs/admin-console.md` (the extension model, under the registry discussion).

## Bind an additional port

**Goal**: your application accepts raw (non-HTTP) connections on its own port.

1. Add the port to the `.dgd` configuration's port list, e.g. `binary_port = ([ "localhost": 8080, "localhost": 8443, "localhost": 8081 ]);`. Despite the mapping-like syntax this is a positional list (the driver's config parser accepts repeated hosts), and an entry's position is the port index managers register against. Index 0 is the HTTP slot, and index 1 -- when a second port is configured -- is the platform's `https` slot (the port-label registry declares that label at boot; `docs/operations.md` Network boundary and transport security), so an application's own raw port starts at index 2.
2. Declare a label for the new port slot and register the manager by label, both from System-tier boot code: `PORTD->declare_label("myapp", "binary", 2)` then `PORTD->register_manager("myapp", manager)` (`PORTD` from `<portd.h>`; telnet-shaped ports declare type `"telnet"`). Both calls are System-tier-gated; the registry refuses loudly where the raw kernel surface is silent -- declaring a label for an unconfigured slot, redeclaring a label, or registering against an undeclared label are all errors, and `query_label` answers what actually landed. The platform's own examples are the System telnet manager (`src/usr/System/sys/userd.c`, label `admin`) and the HTTP bootstrap (`src/usr/System/sys/http_server.c`, label `http`); the canonical labels are declared at boot by `src/usr/System/sys/portd.c`. The underlying kernel surface (`"/kernel/sys/userd"->set_binary_manager(2, manager)`, first-registration-wins) remains available, but a port with no registered manager does not refuse connections -- the kernel silently falls back to its default user object -- so prefer the label path, which turns a forgotten or misaimed registration into an error.
3. Implement the manager contract on the registered object: `select(str)` returns the user object for a new connection, plus `query_mode`, `query_timeout`, and `query_banner`. The contract is specified in `docs/kernel-reference.md` (the userd hooks section).

**Verify**: `code "/usr/System/sys/portd"->query_label("myapp")` on the admin console answers `({ type, index, port, manager })`; then connect a client to the new port -- your manager's `select()` runs (log from it while developing).

**Owning doc**: `docs/kernel-reference.md` userd; `docs/operations.md` for the `.dgd` port configuration fields.

## Expose a health check for monitoring

**Goal**: a monitoring system reads the platform's capacity counts over HTTP, with no console login.

1. Add a status route to your application's HTTP server object: call the no-argument `status()` and emit the capacity-headroom counts (`objects`, callouts, swap sectors, `users`) as stable `key=used/cap` lines. `examples/http-app/obj/server.c`'s `GET /status` route is the worked form -- copy its report block.
2. The route rides your existing `binary_port` mount, cleartext or TLS (`docs/operations.md` Network boundary and transport security); no new port and no operator credential is involved.
3. Point the monitor at the route on an interval and alert on the thresholds in `docs/operations.md` Monitoring signals -- the swap-sector line earliest, because its ceiling is fatal rather than degrading.

**Verify**: `curl http://localhost:8080/status` against the deployed example returns the five `key=value` lines; cross-check a value against the console `status` block.

**Owning doc**: `docs/operations.md` Monitoring signals; `examples/http-app/` for the worked route.

## Make an outbound HTTP request

**Goal**: your code calls another service over HTTP or HTTPS.

1. Inherit `Http1Client` (TLS variant `Http1TlsClient`) composed with `/usr/HTTP/api/lib/BufferedConnection1`, keeping the driver-level connection raw -- the shape `examples/composite-app`'s loopback client (`Inventory/obj/client.c`) uses. That client is the surface's first in-tree consumer; its header documents two of the plain-client path's three latent defects the buffered composition avoids, and the third (a double connect) sits in `obj/client1.c` itself (`docs/application-authoring.md` Outbound connections names all three).
2. The constructor signatures live in `docs/http-applications.md` API signatures; the surface's proven-versus-shipped boundary is stated plainly in `docs/application-authoring.md` Outbound connections.

**Verify**: `scripts/run-example.sh composite-app` drives the client end-to-end over real TCP among its phases.

**Owning doc**: `docs/application-authoring.md` Outbound connections; `docs/http-applications.md` API signatures.

## Encode or decode JSON

**Goal**: convert between LPC values and JSON text.

1. The in-tree idiom is inheritance: `inherit "/lib/util/json";` then `json::encode(value)` / `json::decode(str)` (`src/lib/util/json.c`; every shipped consumer uses this form). The registered singletons `src/sys/jsonencode.c` / `src/sys/jsondecode.c` expose the same pair callable directly, which is what the console probe below uses.
2. The per-class block in `docs/kernel-libraries.md` (Utilities) states the supported value shapes and bounds.

**Verify**: from the console, `code "/sys/jsonencode"->encode((["a": ({1, 2})]))`.

**Owning doc**: `docs/kernel-libraries.md` Utilities.

## Find objects by a field value

**Goal**: answer "which entities have field = X" without walking the whole store.

1. Keep a second mapping beside the store, keyed by the field, updated in the same `atomic` function as every store mutation -- the two writes commit or roll back together, so the index cannot drift (`docs/application-authoring.md` Modeling domain data has the worked form).
2. If the store's writes are dispatched properties you do not own, register an observer on the property instead; it fires synchronously inside the write's atomic envelope (`docs/dispatcher.md`).
3. For name-to-object resolution (not field queries), use logical names: `set_object_name` at create, `find_named` anywhere (`docs/kernel-libraries.md` /lib/util/named.c).

**Verify**: from the console, `code` the daemon's query surface for a known field value and confirm the ids match the store.

**Owning doc**: `docs/application-authoring.md` Modeling domain data.

## Serve HTTPS on the labeled port

**Goal**: the platform terminates TLS 1.3 natively for your application.

1. Provide the three activation pieces: the lpc-ext crypto module in the `.dgd` `modules` mapping, a second `binary_port` entry (the port-label registry declares `https` for it), and PEM credentials at the configured paths. Anything missing is a logged stand-down, not an error.
2. A certificate that lands after boot activates with the console `tls-cert reload`; renewals are just the file copy, read per connection. The host's ACME client owns issuance.
3. `examples/https-app/` is the reference server subclass.

**Verify**: `LPC_EXT_CRYPTO=<module> DGD_BIN=<dgd> scripts/https-smoke.sh` -- the nine-phase end-to-end incl. the statedump key-scans. The smoke logs in as `admin` with the default password `drive-verbs`; if you have claimed the console with your own password, delete `src/kernel/data/admin.pwd` first, or the run fails with `password rejected` (`scripts/README.md`).

**Owning doc**: `docs/operations.md` Network boundary and transport security.

## Register a user and gate an HTTP route

**Goal**: a route that answers only to an authenticated session, in your own transport-facing domain.

1. Issue a challenge from an unauthenticated route: `AUTHD->issue_challenge()` returns the value the client's WebAuthn ceremony signs; consume it single-use at registration (`examples/composite-app/Inventory/sys/handler.c` is the worked form).
2. Register: `AUTHD->register_identity(challenge, clientDataJSON, attestationObject)` verifies the ceremony and returns the subject string and a session token.
3. Gate: parse the bearer token from the `Authorization` header and resolve it with `AUTHD->validate(token)` -- the subject string (`identity:<uuid>`), or nil to refuse with 401. Pass the subject, not the token, into your domain daemons.
4. For an admin-only route, gate in the daemon at the capability choke-point -- `CAPABILITYD->is_allowed(<capability>, subject)`, the subject string being exactly what the store records as a principal -- and let the handler translate the refusal to 403 (the composite example's wipe route).

**Verify**: `LPC_EXT_CRYPTO=<module> EXPECTED_OK=51 DGD_BIN=<dgd> scripts/run-example.sh composite-app` -- the registration, auth-gate, and capability-refusal phases assert exactly this sequence over real TCP.

**Owning doc**: `docs/composite-applications.md` Authenticating a wire request.

## Mint an agent identity and delegate a capability to it

**Goal**: a human controller mints an agent identity, hands it a credential, and delegates a capability that dies with suspension.

1. Mint from the controller's session: `AUTHD->mint_agent_with_token(controllerToken)` returns the agent's uuid and its token -- plaintext once, at mint, never again.
2. The agent logs in with `AUTHD->authenticate_agent_token(agentToken)`, receiving its subject string (`identity:<uuid>`) and its own session token.
3. Delegate: `AUTHD->delegate_capability(controllerToken, uuid, capability)`; the reverse is `undelegate_capability`. The grant traces to the controller edge in the store.
4. Suspend and resume with `AUTHD->suspend_agent` / `resume_agent`: suspension kills the delegated grant; resume restores nothing by itself.

**Verify**: the agent phases of the composite example (mint, list, login, not-own refusal, delegation, suspend/suspended, resume), or `examples/agent-app` with its operator continuation.

**Owning doc**: `docs/identity.md` Agent identities; `docs/composite-applications.md`.

## Serve an HTML page or small web UI

**Goal**: your service answers a route with a file-backed page instead of a string literal.

1. Implement the handler contract's one-shot form and read the file per request: return `({ 200, "OK", "text/html; charset=utf-8", read_file("/usr/<App>/data/page.html"), ([ "Cache-Control" : "no-store" ]) })`, with a 500 when the read returns nil and your 404 fallback for other paths -- `examples/composite-app/Inventory/sys/demo.c` is the whole pattern.
2. What the platform gives you: `read_file` is access-checked against your domain's tree, and the per-request read keeps the server current -- an edited file is what the next request reads. The browser is a second cache the server does not control: without a header saying otherwise, Safari heuristically caches the page and a returning visitor can sit on a stale copy until a hard refresh, which is why step 1's fifth element -- the handler contract's optional mapping of extra response headers (`docs/composite-applications.md`) -- sends `Cache-Control: no-store`.
3. What it does not: no MIME table (state the Content-Type yourself), no caching, no directory serving -- one route, one file, which is exactly the admin-panel and demo-page shape.
4. If the page drives WebAuthn, serve it over the labeled `https` port with an origin matching the relying-party configuration -- the browser requires a secure context (`examples/composite-app/README.md`).

**Verify**: `curl` the route and confirm the Content-Type and body; edit the file and confirm the reload shows it.

**Owning doc**: `docs/http-applications.md`; `examples/composite-app/Inventory/sys/demo.c`.

## Inspect or hot-fix a live object from the console

**Goal**: read a running object's state, or replace its code without a restart.

1. Inspect: `status(<obj>)` for the runtime's per-object vector, `code <expr>` to evaluate against the live image (`docs/admin-console.md` Inspecting runtime state; console verbs resolve Index logical names beside paths).
2. Hot-fix: edit the source file, then `compile <file.c>` -- the master is replaced in place, clone state survives, and a failed compile is a no-op (`docs/admin-console.md` Hot-fixing code in production).
3. If the change alters a clone's data shape, use the cascade with patching instead: `upgrade -p <file.c>` (Migrate live state after a data-shape change above).

**Verify**: `issues <file.c>` confirms the cascade converged; a `code` probe exercises the new behavior.

**Owning doc**: `docs/admin-console.md`; `docs/changing-a-running-system.md` rungs 1-3.

## Run the browser demo

**Goal**: the composite example's guided walk running in your browser over TLS the browser trusts, from one command.

1. Prerequisites, once per machine: `mkcert` with its CA installed (`mkcert -install`), `python3`, `openssl`. The identity ceremonies need the host crypto module.
2. `LPC_EXT_CRYPTO=/path/to/crypto.<ver> DGD_BIN=/path/to/dgd scripts/demo-composite.sh` deploys the interactive shape, generates the certificate, boots with native TLS on the labeled `https` port, drives the bring-up console verbs (two provision -- the provisioner compile and the capability's delegable flag -- the rest verify), and leaves the instance running -- `DEMO READY` (with the server pid) is the success signal. The script refuses to start while another dgd instance is running.
3. Open `https://localhost:8443/demo` and follow the numbered walk: the register / login / recover entry triad, delegation with an observable effect, the intended refusals at all three authorization tiers, and passkey self-service including add-passkey enrollment.
4. Teardown when done: the script's header lists the exact commands -- kill the printed pid, then remove the deployed mounts, the TLS material, the provisioner copy, the demo's state files, and the kernel access-grant residue (`src/kernel/data/access.data`).

**Verify**: the script prints `DEMO READY` with the pid; the page loads without a certificate warning and registration completes against your authenticator.

**Owning doc**: `examples/composite-app/README.md` The browser path; `docs/composite-applications.md` The demo page: the guided walk.

## Reset a development checkout to a clean slate

**Goal**: a from-checkout boot with no residue from prior example runs or operator provisioning.

1. Remove every deployed example mount (`src/usr/<Mount>/` -- the full list is `run-example.sh`'s clean-slate loop, and it includes `WWW`, the same mount name the `first-http-endpoint.md` tutorial uses), plus the tutorial domains the harness does not know about (`src/usr/Pet`, `src/usr/KV`) -- leftover domains re-register on every cold boot. Remove the snapshot pair and swap (`state/snapshot`, `state/snapshot.old`, `state/swap`) and the provisioning residue the console flows create (`src/usr/testop/`, `src/kernel/data/access.data`).
2. For a full reset, also delete the admin credential (`src/kernel/data/admin.pwd`): the next console login re-claims it, and the smoke scripts expect the default password `drive-verbs` there, not one you picked in the tutorials (`scripts/README.md`).
3. Or let the harness do it: every `scripts/drive-verbs-smoke.sh` run performs the mount-and-state reset first; `scripts/run-example.sh` resets the mounts and state files but leaves the operator-provisioning residue (`src/usr/testop/`, `access.data`) in place. Both remove `src/usr/WWW` -- it is in the example-mount list, and a tutorial-authored WWW domain goes with it -- but neither touches `admin.pwd`, `src/usr/Pet`, or `src/usr/KV` (`scripts/README.md`).

**Verify**: `git status --short` shows no untracked deploy artifacts; the next cold boot registers no leftover domains.

**Owning doc**: `scripts/README.md` (the clean-slate steps and the Adding a new example checklist).

## Where to next

- [`docs/application-authoring.md`](application-authoring.md): the pattern reference behind most of these recipes.
- [`docs/first-application.md`](first-application.md): the tutorial that precedes them.
- [`docs/admin-console.md`](admin-console.md): the operator surface the verification steps lean on.
- [`docs/code-lifecycle.md`](code-lifecycle.md): compile, clone, touch, and upgrade mechanics.
