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
2. **Did the write actually go through `set_property`?** `set_raw_property` is the documented bypass the dispatcher uses internally, and it is callable by anyone. `dispatch-trace on`, repeat the write, then check `/usr/Merry/log/dispatch.log` -- a `set_property` write leaves a trace entry; a `set_raw_property` write leaves none.
3. **Is the registration on the right `(path, timing)`?** `observers <obj_path> <path>` lists what is actually registered, by slot and index. A `pre` observer registered where `main` was expected, or a path typo, looks identical to "nothing fired" from the caller's side.
4. **Did the observer source actually compile?** Registration compiles the source immediately -- a parse error throws at `register_observer` time, not at first fire (`docs/observers.md` Registration). If the registering call was wrapped in a `catch()` (`docs/lpc-essentials.md` Error handling), that error may have been silently absorbed; re-issue the same call with `register-observer <obj_path> <path> <timing> <source...>` from the console to see it directly.
5. **Is the caller an approved registrar?** `query-approved-registrars` lists the domains permitted to register across a domain boundary; a cross-domain `register_observer` call from a domain not on that list is refused (`docs/dispatcher.md` `register_observer`). Registering on the application's own objects always passes; registering on another domain's objects does not, without that grant.
6. **Does the host actually inherit the property lib?** `register_observer` throws `MERRY: target ... does not carry the property API` immediately if the target does not inherit `/lib/util/properties` (`src/usr/Merry/sys/merry.c` `_check_property_bearer`) -- so if registration returned without error, this was already satisfied. A direct probe cannot confirm it: `call_other` on a missing function returns nil rather than erroring, so `query_raw_property` looks the same on a non-bearer as an unset property does on a bearer -- trust the registration-time guard, or read the target's `inherit` lines in its source.

If all six check out, `observers <obj_path> <path> <timing> -effective` renders the ancestry-walk view the dispatcher actually fires from -- the ground-truth answer when a descendant's local registration is silently shadowing an ancestor's observer, or the re-enable marker meant to pull the ancestor's observer back in was never set (`docs/observers.md` Query surface; `docs/dispatcher.md` Ancestry walk).

## The working environment, plainly

LPC is edited like C, in whatever editor is at hand -- there is no LPC language server and no step debugger for this platform. The admin console's introspection stands in for one: `status`, `code`, and the dispatcher's `observers` / `batch-status` views are how you look inside a running program instead of attaching a debugger to it (`docs/admin-console.md` Inspecting runtime state).

The iteration loop is short because the platform never leaves the running image: edit the source, `compile <file.c>` to replace the master in place, then use `code <expr>` as a REPL against the freshly compiled object to exercise it directly (`docs/admin-console.md` Hot-fixing code in production). Reach for `dispatch-trace on` only when the misbehavior is inside dispatcher-mediated reactions rather than the code just compiled. Recompiling preserves the object's existing dataspace -- a clone's variables survive the swap (`docs/runtime-primitives.md` Hot reload) -- so iteration does not cost a restart or lost fixture state between edits. A failed compile is a no-op, not an outage: compilation runs in its own atomic context, so a syntax error or failed type-check leaves the previous master untouched (`docs/changing-a-running-system.md` Hot-fix one object) -- fix and recompile again without fear of leaving the object half-updated.

Automated tests are boot-time sentinel drivers, not a separate test runner: a `sys/test.c` in the application domain runs its checks at cold boot and writes a pass/fail line an external script asserts on. See `docs/application-authoring.md` for the pattern.

## Where to next

- [`docs/operations.md`](operations.md) -- Logging and diagnostics: the full errord/logd/console path, threshold semantics, and the dispatcher's audit log.
- [`docs/dispatcher.md`](dispatcher.md) -- the property-change dispatcher reference: registration, timing slots, cascade and cycle bounds, the observer-source contract.
- [`docs/admin-console.md`](admin-console.md) -- the operator surface used throughout this document: `log`, `log-level`, the Dispatcher operator surface verbs, and Inspecting runtime state / Hot-fixing code in production.
