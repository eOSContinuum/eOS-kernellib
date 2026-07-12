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

**Verify**: `issues <file.c>` reads back whether the cascade fully propagated; probe a pre-existing clone's format-version property with `code`.

**Owning doc**: `docs/code-lifecycle.md` Touch; `docs/changing-a-running-system.md` rung 3.

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

1. Add the port to the `.dgd` configuration's port list, e.g. `binary_port = ([ "localhost": 8080, "localhost": 8081 ]);`. Despite the mapping-like syntax this is a positional list (the driver's config parser accepts repeated hosts), and an entry's position is the port index managers register against -- the shipped setup uses only index 0.
2. Register a manager for the new index: `"/kernel/sys/userd"->set_binary_manager(1, manager)` (telnet-shaped ports use `set_telnet_manager`). The call is System-tier-gated and first-registration-wins, so it belongs in System-tier boot code -- the platform's own examples are the System telnet manager (`src/usr/System/sys/userd.c`) and the HTTP bootstrap (`src/usr/System/sys/http_server.c`), each claiming index 0 of its flavor. A port with no registered manager does not refuse connections; the kernel silently falls back to its default user object, so a forgotten registration misbehaves rather than erroring.
3. Implement the manager contract on the registered object: `select(str)` returns the user object for a new connection, plus `query_mode`, `query_timeout`, and `query_banner`. The contract is specified in `docs/kernel-reference/hook/userd`.

**Verify**: connect a client to the new port; your manager's `select()` runs (log from it while developing).

**Owning doc**: `docs/kernel-reference/hook/userd`; `docs/operations.md` for the `.dgd` port configuration fields.

## Where to next

- [`docs/application-authoring.md`](application-authoring.md): the pattern reference behind most of these recipes.
- [`docs/first-application.md`](first-application.md): the tutorial that precedes them.
- [`docs/admin-console.md`](admin-console.md): the operator surface the verification steps lean on.
- [`docs/code-lifecycle.md`](code-lifecycle.md): compile, clone, touch, and upgrade mechanics.
