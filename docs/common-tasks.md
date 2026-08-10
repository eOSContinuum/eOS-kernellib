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

## Walk every object in your domain

**Goal**: run a periodic sweep over every live clone your domain owns -- the "Periodic global touch" mitigation the upgrade doctrine names -- with no platform query to enumerate them by.

1. **State the gap honestly first.** The platform's object manager (`/usr/System/sys/objectd.c`) tracks the compiled-program graph -- path, inheritance, includes, issues -- not clone populations: its query surface (`query_path`, `query_issues`, `query_includes` / `query_included`, `query_inherits` / `query_inherited`) answers "what does this program depend on" and "what depends on it", never "which clones exist" or "which clones does owner X hold". There is no by-owner or per-master enumeration to walk. A domain that needs one tracks its own.
2. **Keep a clone registry.** The owning daemon records each clone's ref at create time and drops it at destruct time, in the same `atomic` function as the write that creates or removes the entity -- the same discipline `docs/application-authoring.md` Modeling domain data already teaches for a field index, applied to the whole population instead of one field:

```c
private mapping clones;             /* id : object */

atomic int spawn()
{
    object clone;
    int id;

    id = nextId++;
    clone = clone_object("obj/thing");
    clones[id] = clone;
    return id;
}

atomic void remove(int id)
{
    object clone;

    clone = clones[id];
    if (clone) {
        clones[id] = nil;
        destruct_object(clone);
    }
}
```

The registry can never disagree with what actually exists, for the same reason a secondary field index cannot: the store mutation and the registry write commit or roll back together.

3. **Sweep the registry in slices.** Walking the whole registry in one task risks `Out of ticks` on a large domain, so slice it the way "Schedule recurring or oversized work" above does -- a bounded chunk per `call_out`, cursor saved between slices, each slice landing under a fresh tick budget:

```c
static void do_sweep(int *ids, int cursor)
{
    int end, i;
    object clone;

    end = (cursor + CHUNK < sizeof(ids)) ? cursor + CHUNK : sizeof(ids);
    for (i = cursor; i < end; i++) {
        clone = clones[ids[i]];
        if (clone) {
            "/usr/System/sys/<yourApp>_touch"->touch(clone);
        }
    }
    if (end < sizeof(ids)) {
        call_out("do_sweep", 0, ids, end);
    }
}
```

4. **Route the touch through a System-tier overlay.** `call_touch` is System-creator-gated (`/kernel/lib/auto.c`), the same gate `dump_state` sits behind, so a tier-E domain cannot call it directly -- confirmed live: an in-domain `call_touch(clone)` refuses with `Permission denied`. Carry a small overlay file the same way the durability recipe ("Make one write durable at acknowledge time" above) routes `dump_state`: a one-method System-tier object your deploy step lands under `src/usr/System/sys/` (`docs/application-repository.md` System-tier overlay files) that does nothing but call `call_touch` on the object it's handed.

**Verify**: against a live boot, spawn several clones, remove one, and confirm the registry's count drops by exactly one and the removed id resolves to nil; run the sweep and confirm it took more than one `call_out` slice for a registry sized past one chunk, and that every surviving clone's `patch()` ran exactly once (a `touched` counter on the clonable, incremented in `patch()`, is the witness) -- verified live at this shape: five clones spawned, one removed (four remain, the removed id nils), a two-slice sweep against a chunk size of two, and every surviving clone's touch counter at 1.

**Owning doc**: `docs/application-authoring.md` Live code upgrade through `call_touch` and Object tracking (the objectd query surface); `docs/code-lifecycle.md` Touch; `docs/common-tasks.md` Find objects by a field value (the index discipline this extends) and Make one write durable at acknowledge time (the System-tier overlay pattern).

## Grant another domain access to your files

**Goal**: domain `Foo` can read (or write) under `/usr/Bar/`.

1. Operationally, from the admin console: `grant Foo /usr/Bar/lib read` (`write` is the default when the mode keyword is omitted; `full` also exists). `ungrant` reverses it, and `access <user>` / `access <directory>` audit the current bits.
2. `grant global <directory>` instead adds a `/usr/`-subdirectory to the global-read set -- the right shape when every domain should read a shared library.
3. At boot time, grants are System-tier acts: the access API (`set_access`) is reachable from System code, not from a tier-E domain's own initd -- a domain cannot grant itself access to anything. Provision them once from the console (grants persist in the kernel's saved access data), or route them through a System-tier overlay file your own repository carries -- `docs/application-repository.md` System-tier overlay files covers that file and the explicit compile step it needs.

**Verify**: `access Foo` lists the grant; a `read_file` from Foo's code stops erroring.

**Owning doc**: `docs/admin-console.md` (Permissions verbs); `docs/application-authoring.md` Owner and access.

## Add an operator verb for your application

**Goal**: an operator at the console can run `myapp-status` instead of a `code` one-liner.

1. Have the operator approve your domain once: `console-ext approve MyApp` on the kernel console grants the `admin_console.extend` capability to the domain principal (`console-ext` with no arguments lists the current entries and approved domains).
2. Supply an object in your domain exposing a `cmd_<verb>(object user, string cmd, string str)` method, and register it from your own code -- typically the initd or daemon at boot: `ADMIN_CONSOLE_REGISTRY->extend("myapp-status", "/usr/MyApp/sys/myappd", "cmd_myapp_status")` (`ADMIN_CONSOLE_REGISTRY` from `<kernel/user.h>`). The contract: the registered path lives in your own domain, the method carries the `cmd_` prefix, and the verb is a single word that shadows no built-in and takes no already-registered name. `retract("myapp-status")` removes it; only the registering domain can. `examples/console-ext-app` is the worked example, including the boot-before-approval pattern.
3. The zero-surface alternative remains a documented `code` call on your own daemon (`code "/usr/MyApp/sys/myappd"->status()`), which needs no approval and no registration.

**Verify**: the new verb answers on the kernel console (`admin` login); `No command` means the entry or the extension object path is wrong, and an `extend` refusal (`capability denied: principal MyApp lacks admin_console.extend`) means the domain is not approved.

**Owning doc**: `docs/admin-console.md` The application registration surface.

## React to a property change with a sandboxed script

**Goal**: code runs automatically, inside the write, when a specific property on one of your objects changes.

1. Give the target object the property API: inherit `/lib/util/properties` (most application objects already carry this for their own state).
2. Register the reaction: `MERRY->register_observer(target, "your:property", "main", "<Merry source>")` -- the Merry source compiles into the sandbox at registration time (a 51-entry kfun deny list; no raw object access outside the provided surface) and is stored as a property on `target` itself, so it persists and replicates with the object like any other state.
3. Pick the timing: `"pre"` runs before the write lands and can veto it, `"main"` runs after the write with the new value visible, `"post"` runs after every `"main"` observer at that triple has fired -- ordering, veto, multi-observer fan-out, and cascade bounds are `dispatcher.md`'s.
4. `register_observer` (and `unregister_observer`, `remove_observer`) pass the registrar gate (`_check_registrar`), which accepts exactly three callers: a program under `/kernel/`, a caller whose creator domain holds the `merry.registrar` capability, or a caller registering on a target in its own domain -- the self-domain path almost every application uses. A cross-domain registration without the capability is refused; grant it via the `approve-registrar` admin verb if your domain genuinely needs to register on objects it does not own.

**Verify**: `DGD_BIN=<dgd> scripts/run-example.sh signal-app` -- `PASS`, 1 `" OK"` sentinel: the worked form of exactly this recipe (one property host, one `"main"` observer, one write, one assertion that the reaction already ran when the write returned).

**Owning doc**: `docs/signal-applications.md` (the walkthrough); `docs/dispatcher.md` (timings, ordering, the registrar gate, cascade bounds); `docs/observers.md` (the observer lifecycle contract).

## Bind an additional port

**Goal**: your application accepts raw (non-HTTP) connections on its own port.

1. Add the port to the `.dgd` configuration's port list, e.g. `binary_port = ([ "localhost": 8080, "localhost": 8443, "localhost": 8081 ]);`. Despite the mapping-like syntax this is a positional list (the driver's config parser accepts repeated hosts), and an entry's position is the port index managers register against. Index 0 is the HTTP slot, and index 1 -- when a second port is configured -- is the platform's `https` slot (the port-label registry declares that label at boot; `docs/operations.md` Network boundary and transport security), so an application's own raw port starts at index 2.
2. Declare a label for the new port slot and register the manager by label, both from System-tier boot code: `PORTD->declare_label("myapp", "binary", 2)` then `PORTD->register_manager("myapp", manager)` (`PORTD` from `<portd.h>`; telnet-shaped ports declare type `"telnet"`). The registry refuses loudly where the raw kernel fallback is silently forgiving; its full contract -- signatures, gating, what errors, the boot-declared canonical labels -- is `docs/system-daemons.md` portd. The platform's own registrations are the worked examples: the System telnet manager (`src/usr/System/sys/userd.c`, label `admin`) and the HTTP bootstrap (`src/usr/System/sys/http_server.c`, label `http`).
3. Implement the manager contract on the registered object: `select(str)` returns the user object for a new connection, plus `query_mode`, `query_timeout`, and `query_banner`. The contract is specified in `docs/kernel-reference.md` (the userd hooks section).

**Verify**: `code "/usr/System/sys/portd"->query_label("myapp")` on the admin console answers `({ type, index, port, manager })`; then connect a client to the new port -- your manager's `select()` runs (log from it while developing).

**Owning doc**: `docs/system-daemons.md` portd (the registry contract); `docs/kernel-reference.md` userd (the manager hooks); `docs/configuration.md` for the `.dgd` port configuration fields.

## Expose a health check for monitoring

**Goal**: a monitoring system reads the platform's capacity counts over HTTP, with no console login.

1. Add a status route to your application's HTTP server object: call the no-argument `status()` and emit the four capacity-headroom counts (`objects`, callouts, swap sectors, `users`) as stable `key=used/cap` lines, plus an `uptime` line (a bare seconds value -- the fifth line the Verify below counts). `examples/http-app/obj/server.c`'s `GET /status` route is the worked form -- copy its report block.
2. The route rides your existing `binary_port` mount, cleartext or TLS (`docs/operations.md` Network boundary and transport security); no new port and no operator credential is involved.
3. Point the monitor at the route on an interval and alert on the thresholds in `docs/operations.md` Monitoring signals -- the swap-sector line earliest, because its ceiling is fatal rather than degrading.

**Verify**: `curl http://localhost:<binary_port>/status` (your configured `binary_port` -- 8080 on an unedited `example.dgd`; `scripts/README.md` Port allocation on a shared machine if something else already holds that port) against the deployed example returns the five `key=value` lines; cross-check the capacity caps and the `users` count against the console `status` block. The used counts (objects, callouts, sectors) legitimately drift a few units between the two probes -- the probe connection and the console login are themselves objects -- so matching caps and `users` is the check, not equal object counts.

**Owning doc**: `docs/operations.md` Monitoring signals; `examples/http-app/` for the worked route.

## Poll the health vector without an HTTP route

**Goal**: a monitoring system reads the capacity counts over the console, for a deployment with no HTTP application to carry a status route -- the HTTP route's five fields in the same vocabulary, plus `swap-rate5`, which only the console path carries.

1. Provision the monitoring credential once: `grant monitor access` from the admin console, with no directory grants, and walk its first login deliberately -- the set-a-password window is first-come (`docs/security-posture.md` Credential lifecycle). The grant creates `src/usr/monitor` (the kernel makes every newly granted user's directory, regardless of whether the grant carries any directory access), so a checkout that has run this recipe carries that empty mount from then on. `docs/operations.md` (Monitoring signals, The monitoring credential) carries the credential's verified surface and its one standing caveat: `halt` has no access gate, so this credential is never read-only in blast radius.
2. Write a verbset for the harness's telnet client (`scripts/drive-verbs.py`; block format in `scripts/README.md`): file-level `user:`/`password:` directives for the credential, then one `code` block emitting the same `key=used/cap` vocabulary the HTTP route uses, with an `expect:` per field:

```text
user: monitor
password: <the credential>

cmd: code (a = status()), "objects=" + a[ST_NOBJECTS] + "/" + a[ST_OTABSIZE] + "\ncallouts=" + (a[ST_NCOSHORT] + a[ST_NCOLONG]) + "/" + a[ST_COTABSIZE] + "\nswap-sectors=" + a[ST_SWAPUSED] + "/" + a[ST_SWAPSIZE] + "\nusers=" + a[ST_NUSERS] + "/" + a[ST_UTABSIZE] + "\nuptime=" + a[ST_UPTIME] + "\nswap-rate5=" + a[ST_SWAPRATE5]
expect: objects=\d+/\d+
expect: callouts=\d+/\d+
expect: swap-sectors=\d+/\d+
expect: users=\d+/\d+
expect: uptime=\d+
expect: swap-rate5=\d+
```

3. The collector runs `python3 scripts/drive-verbs.py <verbset> --port <telnet_port> --transcript <file>` on its polling interval: a nonzero exit is the alert for unreachable, failed login, or a missing field, and the transcript's `$N = "..."` result line carries all six fields in one string (the console prints the LPC string with its newlines escaped, so the collector regexes each `key=` pair out of the one line rather than splitting on real newlines).
4. Map the fields to the threshold table (`docs/operations.md` Monitoring signals): `swap-sectors` earliest (warn 50%, page 70% -- the fatal ceiling), `objects`, `callouts`, and `users` at warn 70% / page 85%, `uptime` pages on any decrease, and `swap-rate5` is the five-minute swap-activity row (warn when nonzero on two consecutive polls, page when still nonzero fifteen minutes later). The one row no field carries -- health-route response time -- is the probe's own latency measurement.

**Verify**: against a booted platform the invocation exits 0 with every `expect:` line PASS; stop the platform and rerun -- nonzero exit is the unreachable alert firing.

**Owning doc**: `docs/operations.md` Monitoring signals (the thresholds and the monitoring credential); `scripts/README.md` for the verbset format and client.

## Make an outbound HTTP request

**Goal**: your code calls another service over HTTP or HTTPS.

1. Inherit `Http1Client` (TLS variant `Http1TlsClient`) composed with `/usr/HTTP/api/lib/BufferedConnection1`, keeping the driver-level connection raw -- the shape `examples/composite-app`'s loopback client (`Inventory/obj/client.c`) uses. That client is the surface's first in-tree consumer; its header documents two of the plain-client path's three latent defects the buffered composition avoids, and the third (a double connect) sits in `obj/client1.c` itself (`docs/http-applications.md` Outbound connections names all three).
2. The constructor signatures live in `docs/http-applications.md` API signatures; the surface's proven-versus-shipped boundary is stated plainly in `docs/http-applications.md` Outbound connections.

**Verify**: `scripts/run-example.sh composite-app` drives the client end-to-end over real TCP among its phases.

**Owning doc**: `docs/http-applications.md` Outbound connections; `docs/http-applications.md` API signatures.

## Encode or decode JSON

**Goal**: convert between LPC values and JSON text.

1. The in-tree idiom is inheritance: `inherit "/lib/util/json";` then `json::encode(value)` / `json::decode(str)` (`src/lib/util/json.c`; every shipped consumer uses this form). The registered singletons `src/sys/jsonencode.c` / `src/sys/jsondecode.c` expose the same pair callable directly, which is what the console probe below uses.
2. The per-class block in `docs/kernel-libraries.md` (Utilities) states the supported value shapes and bounds.

**Verify**: from the console, `code "/sys/jsonencode"->encode((["a": ({1, 2})]))` answers `$N = "{\"a\":[1,2]}"` -- the console renders the returned string as an LPC literal, so the escaped quotes are the success shape, not double encoding.

**Owning doc**: `docs/kernel-libraries.md` Utilities.

## Find objects by a field value

**Goal**: answer "which entities have field = X" without walking the whole store.

1. Keep a second mapping beside the store, keyed by the field, updated in the same `atomic` function as every store mutation -- the two writes commit or roll back together, so the index cannot drift (`docs/application-authoring.md` Modeling domain data has the worked form).
2. If the store's writes are dispatched properties you do not own, register an observer on the property instead; it fires synchronously inside the write's atomic envelope (`docs/dispatcher.md`).
3. For name-to-object resolution (not field queries), use logical names: `set_object_name` at create, `find_named` anywhere (`docs/kernel-libraries.md` /lib/util/named.c).

**Verify**: from the console, `code` the daemon's query surface for a known field value and confirm the ids match the store.

**Owning doc**: `docs/application-authoring.md` Modeling domain data.

## Remove an entity and everything that points at it

**Goal**: destruct a live entity so no lookup, index, or on-disk residue resurrects it -- the delete side of the find-and-index recipe above.

1. In one `atomic` function on the owning daemon, remove the store row and any secondary-index entries, then `destruct_object(entity)` -- the mirror of the create side, so the rows and the destruct commit or roll back together. The rollback is total (verified live): an error after the destruct restores the object, its rows, and even its logical-name registration.
2. What cleans itself: the logical-name registration -- the kernel destruct hook clears the Index entry, so `find_named` answers nil with no explicit `set_object_name(nil)` (`docs/kernel-libraries.md` /lib/util/named.c) -- and the entity's own observer registrations, which live in its property table and die with it (`docs/observers.md` Persistence and end-of-life). In-image references other objects still hold resolve to nil after the destruct (`docs/code-lifecycle.md` Destruct: removal).
3. What does not clean itself: Vault on-disk XML. The Vault daemon has no delete API, a stored file survives destruct, and any later respawn silently resurrects the entity from it, state intact (verified live). Removal is deliberately outside the owning domain's reach -- tier-E `remove_file` against the Vault storage root is refused -- so it is an operator act: the console `rm` verb against the store path (the `store` call returned it; the shape is `/usr/Vault/data/vault/<name-with-colons-as-slashes>.xml`), or host-side removal, or System-tier code a deployment deliberately provisions.
4. If other Vault-stored entities hold `lpc_obj` references to the removed one, their stored XML now dangles, and a dangling reference costs the referrer its whole import at the next respawn (`docs/vault-applications.md` Cross-object references). Remove leaf entities before their referrers, and re-`store` any referrer you edited to drop the reference.

**Verify**: from the console, `find_named("<name>")` returns nil and the daemon's field-index probe comes back empty; for a Vault-participating entity, also confirm the store path is gone -- a `spawn_one_by_name` attempt then errors with `no such file` instead of resurrecting.

**Owning doc**: `docs/application-authoring.md` Modeling domain data for the entity lifecycle; `docs/vault-applications.md` for the on-disk store.

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

**Verify**: `LPC_EXT_CRYPTO=<module> EXPECTED_OK=53 DGD_BIN=<dgd> scripts/run-example.sh composite-app` -- the registration, auth-gate, and capability-refusal phases assert exactly this sequence over real TCP.

**Owning doc**: `docs/composite-applications.md` Authenticating a wire request.

## Bind a session to a browser with a cookie

**Goal**: a browser-served route keeps a user logged in across page reloads, instead of holding the bearer token in page JS (the shape "Register a user and gate an HTTP route" above uses).

1. Mint the session the same way the bearer flow does: a ceremony through `AUTHD` (`examples/composite-app/Inventory/sys/handler.c`'s `/auth/register` and `/auth/login`) returns a subject and a bearer token. Nothing here changes on the minting side -- the binding to the client is the only thing that differs.
2. On the response that hands the client its token, append the handler contract's optional fifth element (the extra-headers mapping the Cache-Control recipe above already uses) instead of putting the token in the JSON body: `({ 200, "OK", "application/json", body, ([ "Set-Cookie" : "session=" + token + "; Path=/; HttpOnly; SameSite=Strict" ]) })`. `HttpOnly` keeps the value out of page JS; `SameSite=Strict` is the cheapest cross-site mitigation available at this layer (see the CSRF caveat below); add `Secure` once the route is HTTPS-only (`docs/common-tasks.md` Serve HTTPS on the labeled port).
3. On the return path, read the request's `Cookie` header and split it yourself: `HttpFields` has no dedicated `Cookie` case, so a generic header falls through `RemoteFields.c`'s default branch and parses as a comma-separated list -- `request->headerValue("Cookie")` answers an **array** of list-items, not a string. Join it back (`implode(values, "; ")`) before splitting on `;` into `name=value` pairs, the same delimiter the wire uses between cookie-pairs. Look up the pair you set (`session`) and validate exactly as the bearer flow does: `AUTHD->validate(token)` for the subject, or nil to refuse.
4. State the CSRF consequence plainly, because the bearer flow does not carry it and a cookie-bound route inherits it the moment credentials travel automatically: a browser attaches cookies to a request it did not initiate on your origin (a form post or fetch from another page), so any state-changing route reachable this way needs a CSRF defense the platform does not ship -- a same-site cookie (step 2) blocks the common case, but a belt-and-suspenders application also checks a per-request token the attacker's page cannot read, or requires a custom header (fetch cannot set one cross-origin without triggering CORS preflight, which itself refuses without an allow-listed origin). The bearer pattern above sidesteps this entirely: nothing attaches an `Authorization` header automatically, so cross-site requests arrive unauthenticated.

**Verify**: `curl -i` the minting route and confirm the `Set-Cookie` header carries the token; `curl -i -b <(echo "session=<token>")` (or a cookie jar: `curl -c jar.txt <mint-route>` then `curl -b jar.txt <protected-route>`) against the protected route and confirm it answers with the subject; a request with no cookie or a bogus one gets 401. The CSRF mitigation itself is asserted only by inspection here (SameSite attribute present, no per-request token issued) -- no same-origin-vs-cross-origin browser probe backs it in this recipe.

**Owning doc**: `docs/identity.md` Sessions; `docs/http-applications.md` The routed-handler contract; `docs/application-authoring.md` Identity and request authentication (What an application still builds).

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

## Provision an application secret out of source

**Goal**: an API key or comparable secret your application needs, surviving a cold boot, absent from the source tree, and leaving nothing in the statedump beyond its use.

1. Put the secret in a host file under your domain's data directory -- deploy state, not source: `src/usr/<App>/data/api-key.secret`, mode 0600, owned by the service user. Add the path to your application repository's ignore file (`docs/application-repository.md` The split). This is the platform's own precedent: the kernel's credentials and access bits are file-backed under `src/kernel/data/` for exactly these properties (`docs/security-posture.md`).
2. Read it at use time with `read_file` (access-checked to your own tree) rather than loading it into a long-lived global at boot: a value read, used, and dropped in one task leaves nothing for the statedump to retain. If the daemon must hold it, clear the variable the moment its use ends -- the transient-secret discipline (`docs/security-posture.md`).
3. Rotation is a file replacement (plus a re-read if held). A cold boot needs no step: the file survives it -- the property a console-set, image-only secret lacks: a cold boot rebuilds from source, and nothing else is carried over (the cold-boot row of `docs/operations.md` Availability and data-loss model).

**Verify**: cold-boot and confirm the consumer works with no console provisioning step; `git status --short` shows no secret file; if the secret is ever held in a long-lived variable, scan a fresh statedump for its bytes (the TLS key-scan in `scripts/https-smoke.sh` is the model).

**Owning doc**: `docs/security-posture.md` (the secrets discipline); `docs/operations.md` Day 0: standing up a production deployment.

## Make one write durable at acknowledge time

**Goal**: a write your application has acknowledged to its client survives an unclean stop -- not just until the next scheduled snapshot.

1. Price the three options first (`docs/evaluating.md` Adoption risks, priced -- the durability bullet): a snapshot on the critical path (step 2), an edge-file copy of the one record (step 3), or an external system of record (not a platform recipe). This recipe is the mechanics of the two in-platform options.
2. **Snapshot on the critical path.** In the function that commits the write: mutate, request the snapshot, and defer the acknowledgment to a `call_out` -- never send it from the same task. The timing is the entire correctness story. `dump_state` only registers the request; the driver writes the image when the task ends (statedumps run between timeslices, `docs/persistence.md` The statedump cycle), and at that boundary it flushes queued network output BEFORE it writes the snapshot (the driver's task-end sequence in `dgd.cpp` `endTask`: the flush precedes the deferred dump). An acknowledgment sent from the mutating task can therefore reach the client moments before the image lands, and an unclean stop in that window loses an acknowledged write. A `call_out` runs as a later task, strictly after the dump completes: acknowledge from there. Two gates ride along: `dump_state` is System-creator-gated (`/kernel/lib/auto.c`), so a tier-E domain requests the snapshot through the platform's dump-only surface -- `persist_helper->trigger_dump()`, granted per domain with `capability grant persist.snapshot <domain>` (`docs/persistence.md` The programmatic surface; before that surface existed, this took a System-tier overlay daemon, `docs/application-repository.md`) -- and the cost is the measured dump pause per acknowledged batch (`docs/operations.md` Availability and data-loss model).
3. **Edge-file the one record.** Persist the record itself to a host file under your domain tree at write time, and let the statedump lag: the platform's own idiom for its credentials and access bits (`src/kernel/sys/access_daemon.c` persists via `save_object`; `docs/security-posture.md`). Use `save_object` for a record-holding object or `write_file` for an append shape -- the driver refuses both inside an `atomic` function ("save_object() within atomic function"), so the write sits in plain non-atomic code, or defers by one tick with the coalesced-`call_out(0)` pattern `logd` uses (`docs/operations.md` Logging and diagnostics). A cold boot re-reads the file at use time, the same read-at-use discipline as the secret recipe above.

**Verify**: against a live boot, drive the critical-path write, wait for the acknowledgment to arrive, `kill -9` the driver, restore from the snapshot pair, and read the record back: it is present. (An acknowledgment sent from the mutating task instead is the documented wrong order -- the driver's task-end sequence, not a race you must win, is the evidence.)

**Owning doc**: `docs/persistence.md` The statedump cycle; `docs/evaluating.md` Adoption risks, priced (the three options).

## Run the browser demo

**Goal**: the composite example's guided walk running in your browser over TLS the browser trusts, from one command.

1. Prerequisites, once per machine: `mkcert` with its CA installed (`mkcert -install`), `python3`, `openssl`. The identity ceremonies need the host crypto module.
2. `LPC_EXT_CRYPTO=/path/to/crypto.<ver> DGD_BIN=/path/to/dgd scripts/demo-composite.sh` deploys the interactive shape, generates the certificate, boots with native TLS on the labeled `https` port, drives the bring-up console verbs (two provision -- the provisioner compile and the capability's delegable flag -- the rest verify), and leaves the instance running -- `DEMO READY` (with the server pid) is the success signal. The script is deliberately fixed to the stock 8023/8080/8443 ports (it is the teaching and demo surface, `scripts/README.md` Port allocation on a shared machine) and refuses to start with a clear message when another instance already holds one of them; free the port (stop the other instance, or find it with `lsof -iTCP:8443 -sTCP:LISTEN`) rather than editing the script.
3. Open `https://localhost:8443/demo` and follow the numbered walk: the register / login / recover entry triad, delegation with an observable effect, the intended refusals at all three authorization tiers, and passkey self-service including add-passkey enrollment.
4. Teardown when done: the script's header lists the exact commands -- kill the printed pid, then remove the deployed mounts, the TLS material, the provisioner copy, the demo's state files, and the kernel access-grant residue (`src/kernel/data/access.data`).

**Verify**: the script prints `DEMO READY` with the pid; the page loads without a certificate warning and registration completes against your authenticator.

**Owning doc**: `examples/composite-app/README.md` The browser path; `docs/composite-applications.md` The demo page: the guided walk.

## Restructure your domain layout

**Goal**: rename a domain, or split one domain into several, without leaving broken references or resurrectable residue under the old name.

1. **Know what is entangled with the domain name.** The name is not just a directory: it is the owner and creator string every object under `src/usr/<Domain>/` derives from (`application-authoring.md` Owner, creator, and clone identity), the key under which access grants are recorded (`common-tasks.md` Grant another domain access to your files), the path prefix Vault-stored XML lands under -- `/usr/Vault/data/vault/<Domain>/...` (`vault-applications.md` On-disk shape; the literal prefix is in `operations.md`'s backup table) -- the convention embedded in every logical name the domain registered through `set_object_name("Domain:...")` and resolvable via `find_named` through the Index daemon (`kernel-libraries.md` `/lib/util/named.c`), the namespace half of any per-app schema the domain registered (`node->set_name("Domain", ...)`, `vault-applications.md` Schema registration), and any observer registrations the domain's objects hold -- these die with the objects and are re-created by nothing: observer slots have no export path at all (`persistence.md` Getting data out names them as one of the two categories the codec refuses), so the rebuilt objects come up unobserved until application code re-registers them (`observers.md` Persistence and end-of-life; Registration).
2. **Prefer the export-and-reimport cold path.** Deploy the domain's tree under the new name, cold-boot so the new owner and initd register (`operations.md` Day 0: standing up a production deployment), then respawn each Vault-participating object from its stored XML into the new domain's own context -- the same drill the state-ceiling ladder's terminal move runs deliberately (`operations.md` When the image approaches the state ceiling; `persistence.md` Getting data out; `vault-applications.md` The owning-domain respawn). This is a cold rebuild, not a hot move: it carries only schema-exported state, in the schema-registration order (referenced objects before referrers, `vault-applications.md` Cross-object references), and re-registers each logical name under the new domain's own `set_object_name` calls.
3. **State honestly what a live in-image move can and cannot carry.** Renaming the directory and re-pointing the initd's `compile_object` calls at boot changes the owner going forward, but it does not touch what already resolved under the old name: stored Vault XML paths, prior access grants, and prior Index registrations all still say the old domain, and nothing rewrites them in place. The non-Vault object graph, any pending `call_out`s, and every observer registration never survive a domain rename at all -- none of them is exported by anything (`persistence.md` Getting data out) -- so a live move that must carry them is not available; a rebuild via step 2 is the honest answer whenever more than the Vault-schematized state matters.
4. **Clean up the residue under the old name.** `ungrant` every grant recorded against the old domain (`common-tasks.md` Grant another domain access to your files, the `access <directory>` audit finds them), remove the old domain's Vault XML tree once step 2's respawn has re-stored everything under the new name (an operator `rm` against the store path, the same removal `common-tasks.md` Remove an entity and everything that points at it uses for a single entity), and remove the old mount (`src/usr/<OldDomain>/`) so a stray cold boot does not re-register it.

**Verify**: cold-boot the new domain and read its sentinel driver's result log for the expected OK count (`application-authoring.md` Testing your application); grep `system.log` for `Warning:: Schema node` and `VAULT: Configuration failed` and confirm neither appears for the migrated objects; `access <OldDomain>` on the console reports no remaining grants.

**Owning doc**: `vault-applications.md` The owning-domain respawn and Cross-object references; `operations.md` When the image approaches the state ceiling; `persistence.md` Getting data out.

## Reset a development checkout to a clean slate

**Goal**: a from-checkout boot with no residue from prior example runs or operator provisioning.

1. Remove every deployed example mount (`src/usr/<Mount>/` -- the full list is `run-example.sh`'s clean-slate loop, and it includes `WWW`, the same mount name the `first-http-endpoint.md` tutorial uses), plus the tutorial domains the harness does not know about (`src/usr/Pet`, `src/usr/KV`), plus any mount a credential grant created (`src/usr/monitor` from `grant monitor access`, `src/usr/testop` from the operator-provisioning recipes, and any other user you registered -- `grant <user> access` makes `src/usr/<user>` unconditionally) -- leftover domains re-register on every cold boot. Remove the snapshot pair and swap (`state/snapshot`, `state/snapshot.old`, `state/swap`) and the provisioning residue the console flows create (`src/usr/testop/`, `src/kernel/data/access.data`).
2. For a full reset, also delete the admin credential (`src/kernel/data/admin.pwd`): the next console login re-claims it, and the smoke scripts expect the default password `drive-verbs` there, not one you picked in the tutorials (`scripts/README.md`).
3. Or let the harness do it: every `scripts/drive-verbs-smoke.sh` run performs the mount-and-state reset first; `scripts/run-example.sh` resets the mounts and state files but leaves the operator-provisioning residue (`src/usr/testop/`, `access.data`) in place. Both remove `src/usr/WWW` -- it is in the example-mount list, and a tutorial-authored WWW domain goes with it -- but neither touches `admin.pwd`, `src/usr/Pet`, or `src/usr/KV` (`scripts/README.md`).

**Verify**: `git status --short` shows no untracked deploy artifacts (an `example.dgd` you localized in place shows as ` M` -- that is your configuration, not residue; the generated-copy path in `docs/getting-started.md` Boot it yourself leaves even that clean); the next cold boot registers no leftover domains.

**Owning doc**: `scripts/README.md` (the clean-slate steps and the Adding a new example checklist).

## Where to next

- [`docs/application-authoring.md`](application-authoring.md): the pattern reference behind most of these recipes.
- [`docs/first-application.md`](first-application.md): the tutorial that precedes them.
- [`docs/admin-console.md`](admin-console.md): the operator surface the verification steps lean on.
- [`docs/code-lifecycle.md`](code-lifecycle.md): compile, clone, touch, and upgrade mechanics.
