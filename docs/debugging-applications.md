# Debugging applications

There is no attached debugger on this platform, and none is needed for most problems: the runtime already tells you what broke and where its own introspection surfaces let you look at the running image directly. This document is the application author's path from a symptom -- a bad trace, a missing log line, a reaction that never fired -- to the cause.

**Audience**: an application author whose code is misbehaving; comfortable with LPC (`docs/lpc-essentials.md`) and running the platform locally (`docs/getting-started.md`).

## Reading an error trace

An uncaught runtime error lands as a formatted trace: the error message on its own line, then one line per stack frame -- source line number and function name -- grouped under an object-name header wherever the frame's object changes across the stack (`src/usr/System/sys/errord.c` `runtime_error`). It goes to whatever connection triggered the call, or the boot log if none is attached.

A compile failure is one line instead:

```text
/usr/App/obj/thing.c, 42: undeclared variable count
```

This is what a broken `initd.c` reports during the cold-boot cascade, and what `compile <file.c>` reports from the console.

The `[atomic]` suffix on an error message means the failure happened inside (or under) an `atomic` function and the rollback fired: every dataspace mutation since the atomic envelope was entered was undone, as if the call never happened (`docs/lpc-essentials.md` Atomicity). Its *absence* on a trace from code that mutates state is worth reading literally -- without an `atomic` ancestor on the call stack, that mutation is not undone. A `[caught]` suffix (a separate report from a different driver hook -- the two markers do not combine on one line) means a `catch()` further up the call stack intercepted the error -- it was still reported, but propagation stopped there.

## Where diagnostics go

Every runtime error, atomic-context error, and compile error the driver detects dispatches to `errord` (`src/usr/System/sys/errord.c`, registered via `set_error_manager()`), which formats the trace, sends it to the console immediately, and tees a copy into `logd` at ERROR severity so it survives past the connection that saw it. `logd` (`src/usr/System/sys/logd.c`) is the platform's one diagnostic sink: it buffers each line in memory (`write_file` is illegal inside the atomic contexts most diagnostics fire from) and flushes on the next non-atomic tick to `/usr/System/log/system.log`, one timestamped line per entry:

```text
Jul  5 14:02:11 ERROR Division by zero
```

Application code reaches the same sink through three calls (`/lib/util/lpc.c`), each fixed to a severity:

| Call | Level | Visible at default threshold? |
|---|---|---|
| `debugLog(str)` | DEBUG | No |
| `info(str)` | INFO | Yes |
| `sysLog(str)` | NOTICE | Yes -- also echoed live to the console |

`logd`'s emission threshold defaults to INFO, so `debugLog` output is silently dropped until an operator lowers it. Two admin_console verbs are the operator surface (`docs/operations.md` Logging and diagnostics): `log [N]` tails the last N lines of `system.log` (default 40); `log-level [LEVEL]` reads, or with `debug|info|notice|error` sets, the threshold. When a diagnostic call seems to vanish, check `log-level` before assuming the call never ran.

## The observer did not fire

A property write completed but the expected Merry reaction never ran. Work down this list; each step is one console command.

1. **Is the Merry daemon loaded?** `code find_object("/usr/Merry/sys/merry")`. If nil, every `set_property` call falls straight through to a direct write with no dispatch at all (`docs/dispatcher.md` Property-layer hook architecture) -- no observer anywhere can fire until it loads.
2. **Did the write actually go through `set_property`?** `set_raw_property` is the documented bypass the dispatcher uses internally, and it is callable by anyone. `dispatch-trace on` plus `log-level debug` (trace lines emit at DEBUG; the default INFO threshold would drop them), repeat the write, then `log` -- a `set_property` write leaves a `MERRY trace:` line; a `set_raw_property` write leaves none.
3. **Is the registration on the right `(path, timing)`?** `observers <obj_path> <path>` lists what is actually registered, by slot and index. A `pre` observer registered where `main` was expected, or a path typo, looks identical to "nothing fired" from the caller's side.
4. **Did the observer source actually compile?** Registration compiles the source immediately -- a parse error throws at `register_observer` time, not at first fire (`docs/observers.md` Registration). If the registering call was wrapped in a `catch()` (`docs/lpc-essentials.md` Error handling), that error may have been silently absorbed; re-issue the same call with `register-observer <obj_path> <path> <timing> <source...>` from the console to see it directly.
5. **Is the caller an approved registrar?** `query-approved-registrars` lists the domains permitted to register across a domain boundary; a cross-domain `register_observer` call from a domain not on that list is refused (`docs/dispatcher.md` `register_observer`). Registering on the application's own objects always passes; registering on another domain's objects does not, without that grant.
6. **Does the host actually inherit the property lib?** `register_observer` throws `MERRY: target ... does not carry the property API` immediately if the target does not inherit `/lib/util/properties` (`src/usr/Merry/sys/merry.c` `_check_property_bearer`) -- so if registration returned without error, this was already satisfied. A direct probe cannot confirm it: `call_other` on a missing function returns nil rather than erroring, so `query_raw_property` looks the same on a non-bearer as an unset property does on a bearer -- trust the registration-time guard, or read the target's `inherit` lines in its source.

If all six check out, `observers <obj_path> <path> <timing> -effective` renders the ancestry-walk view the dispatcher actually fires from -- the ground-truth answer when a descendant's local registration is silently shadowing an ancestor's observer, or the re-enable marker meant to pull the ancestor's observer back in was never set (`docs/observers.md` Query surface; `docs/dispatcher.md` Ancestry walk).

## The working environment, plainly

LPC is edited like C, in whatever editor is at hand -- there is no LPC language server and no step debugger for this platform. The admin console's introspection stands in for one: `status`, `code`, and the dispatcher's `observers` / `batch-status` views are how you look inside a running program instead of attaching a debugger to it (`docs/admin-console.md` Inspecting runtime state).

The iteration loop is short because the platform never leaves the running image: edit the source, `compile <file.c>` to replace the master in place, then use `code <expr>` as a REPL against the freshly compiled object to exercise it directly (`docs/admin-console.md` Hot-fixing code in production). Reach for `dispatch-trace on` only when the misbehavior is inside dispatcher-mediated reactions rather than the code just compiled. Recompiling preserves the object's existing dataspace -- a clone's variables survive the swap (`docs/runtime-primitives.md` Hot reload) -- so iteration does not cost a restart or lost fixture state between edits. A failed compile is a no-op, not an outage: compilation runs in its own atomic context, so a syntax error or failed type-check leaves the previous master untouched (`docs/changing-a-running-system.md` Hot-fix one object) -- fix and recompile again without fear of leaving the object half-updated.

Automated tests are boot-time sentinel drivers, not a separate test runner: a `sys/test.c` in the application domain runs its checks at cold boot and writes a pass/fail line an external script asserts on. See `docs/application-authoring.md` for the pattern.

## Error messages

The fastest path into this doc set is often the error string itself. This table indexes the messages an application author is likely to be holding when they arrive here, and where each one is actually documented.

| Message or symptom | Meaning | First action | Where it is documented |
|---|---|---|---|
| `Out of ticks` | The calling owner's tick budget for this execution ran out. | Check `quota <owner>` for the ticks ceiling. | `docs/application-authoring.md` Writing tick-aware code |
| `merry: cascade depth N exceeded at <object>:<path>` | A recursive observer chain hit the dispatcher's cascade-depth bound (default 32) inside one batch. | Read the batch-status entry, then find the observer that re-triggers itself. | `docs/dispatcher.md` `set_max_cascade_depth` |
| `merry: observer cycle detected at <object>:<path>` | The dispatcher's cycle chain saw the same object-and-path pair twice in one execution. | Read the batch-status log for the cycle chain. | `docs/dispatcher.md` `dispatch_set` |
| `MERRY: target <object> does not carry the property API (inherit /lib/util/properties)` | `register_observer` refused a target that does not inherit the property library. | Add `inherit "/lib/util/properties"` to the target. | This document, The observer did not fire, step 6 |
| `Access denied` | The caller failed a capability or access-daemon check: no write access to the path, a kernel-path restriction, a cross-domain refusal. | Check the access rule the operation cites (write access, domain grant). | `docs/kernel-reference/efun/make_dir` ERRORS |
| `Permission denied` | The call ran with no current object. A calling-convention violation, not a capability denial. | Check what invoked the call; it needs a current-object context. | `docs/kernel-reference/efun/make_dir` ERRORS |
| `capability denied: principal <p> lacks <c>` | The capability store's `require_member` refused a principal that lacks the named capability. | Grant the capability from a `/kernel/*` program, or confirm the principal string is right. | `docs/capability.md` The mechanism |
| `File quota exceeded` | The file creator's file-block quota is already at its limit. | Check `quota <owner>` for the fileblocks ceiling. | `docs/kernel-reference/efun/make_dir` ERRORS |
| `Cannot clone <path>` | `clone_object` ran against a path with no owner, no compiled master, an instance path, or a `/lib/` path. | Confirm the target is a compiled `/obj/` master, not a library. | `docs/application-authoring.md` Domain layout |
| `Cannot create new instance of <path>` | `new_object` ran against an uncompiled master or a `/lib/` path. | Compile the master object first. | `docs/xml.md` Boot sequence |
| Stack overflow (unbounded recursion) | An HTTP application's override chained to `::receiveRequest` on the same relay object, recursing without bound. | Remove the `::receiveRequest` chain call. Override; do not chain. | `docs/http-applications.md` Never call `::receiveRequest` |
| `Hotbooting is disabled` | `shutdown(1)` ran with no `hotboot` tuple in the `.dgd` config. | Add the `hotboot` tuple, or use `shutdown()` for a cold reboot. | `docs/persistence.md` Hot boot: snapshot + execv + fd inheritance |
| `Missing secondary snapshot` | A two-file restore ran with only the primary (incremental) dump file, missing its base. | Restore with both `dump_file` and `dump_file.old` named on the command line. | `docs/operations.md` Backing up and restoring state |
| `[atomic]` / `[caught]` trace suffix | `[atomic]` means the rollback fired and every mutation since the atomic envelope was undone. `[caught]` means a `catch()` further up the stack intercepted the error. | Read the section above before assuming the mutation stuck, or that nothing handled the error. | This document, Reading an error trace |
| Compile failure, one line (`path, line: message`) | A `compile <file.c>` call, or an `initd.c` load during the cold-boot cascade, failed to compile. | Fix the named file and line, then recompile or reboot. | This document, Reading an error trace |

**What an initd compile failure does to the rest of the boot.** `System`'s `initd.c` loads each domain's `initd.c` with no `try`/`catch` around the compile (`src/usr/System/initd.c`, the domain loop calling `load()`, which wraps `compile_object` directly). The driver's `_initialize()` calls into `System`'s `initd` the same way, also uncaught (`src/kernel/sys/driver.c`). The only handler on this path is the outer `initialize()` function, whose `try`/`catch(...)` prints `Initialization failed.` and calls `shutdown()`. A compile failure in one domain's `initd.c` is not contained to that domain: it aborts the entire cold boot.

## Where to next

- [`docs/operations.md`](operations.md) -- Logging and diagnostics: the full errord/logd/console path, threshold semantics, and the dispatcher's audit log.
- [`docs/dispatcher.md`](dispatcher.md) -- the property-change dispatcher reference: registration, timing slots, cascade and cycle bounds, the observer-source contract.
- [`docs/admin-console.md`](admin-console.md) -- the operator surface used throughout this document: `log`, `log-level`, the Dispatcher operator surface verbs, and Inspecting runtime state / Hot-fixing code in production.
