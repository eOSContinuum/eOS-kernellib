# Merry Language

Merry is the safe-sublanguage of LPC used inside eOS-kernellib for code that is loaded, compiled, and executed at runtime under a sandbox. A Merry source string is parsed by a yacc grammar derived from LPC's, translated to LPC, and compiled into a clonable object whose inheritor chain installs the sandbox. The result is an LPC object whose body cannot reach the kfuns the sandbox forbids and whose extensions over LPC (`$arg` argument refs, `${name}` object refs, `$delay()` continuation, `space::method(...)` cross-namespace calls) compile down to property reads, writes, and runtime LFUN dispatches against the binding host.

For depth on LPC itself, see [lpc-essentials.md](lpc-essentials.md). For the application-author perspective (writing a script-bearing object, binding a script to a property, invoking it from LPC), see [merry-applications.md](merry-applications.md). This document is the language-author perspective: writing Merry source.

**Audience**: a programmer who already reads LPC and now needs to write or read Merry source. Familiar with the LPC type system, atomic-by-default call semantics, and the `obj->lfun()` call shape. Reading this to learn what changes when the language is restricted to the sandboxed subset.

## What is familiar

Most of LPC's surface carries over. Curly braces, semicolons, `if` / `while` / `for` / `do-while` / `switch` / `case` / `default` / `break` / `continue` / `return`, the standard arithmetic, relational, logical, and bitwise operators, the assignment-modifier family (`+=`, `-=`, `*=`, etc.), prefix and postfix `++` / `--`, conditional expressions (`? :`), the array literal `({ a, b, c })`, the mapping literal `([ k : v ])`, `catch` (with the same atomic-context-rollback semantics LPC has), and the standard arithmetic / string / mapping kfun set are all available in expressions and statements.

Merry source compiles to LPC source by the parser-runner pair (`/lib/util/parse`, `/lib/util/fileparse`) plus the `data/merry.c` AST-to-LPC translator. The LPC that results is itself subject to the host driver's atomicity, hot-reload, and statedump semantics, so a Merry script is a first-class LPC object once compiled.

## What is restricted

Restrictions exist at three layers: grammar (the parser refuses the form), translator (the AST-to-LPC step refuses the construct), and sandbox (the compiled object's local namespace shadows the kfun and raises at runtime).

**Cross-object call uses `Call()`, not `->`.** The `obj->func(args)` operator is forbidden at the grammar level (`grammar/merry.y` line 325). A Merry source that contains `->` raises a parse error. The replacement is the `Call(obj, "func", "key", value, ...)` merryfun, which dispatches through `merrynode.c::Call` and obeys the sandbox's filter on which methods may be called on which objects.

**No `rlimits` blocks.** The `rlimits` statement is grammar-recognized and immediately errors (`grammar/merry.y` line 186). Merry code runs under the binding host's resource budget; a script cannot escalate its own tick or call budget.

**No local variable declarations.** Merry is an expression/statement language closer to TCL than to C: `object o = Spawn($this); ...; return Set(o, "name", "x");` does not parse. The grammar permits only `constant_declaration` (`constant IDENT = expression;`) for compile-time constants and `local_data_declaration` for parameter-like declarations at the top of a `compound_stmt`. State threads through `args` (the inherited argument mapping) and through properties on the binding host; intermediate values compose inline. The reference application's `examples/merry-app/sys/test.c` phase 3 illustrates: `"return object_name(Spawn($this));"` rather than a multi-statement let-and-return.

**Adjacent string literals do not concatenate.** Plain LPC permits `"foo" "bar"` to mean `"foobar"`. Merry does not. The translator's `imp()` pass (`grammar/merry.y` lines 36-69) merges adjacent string elements only in argument-list positions, not in the general expression position. For cross-line string composition, use explicit `+`: `"first " + "second"`.

**Narrow allowed-kfun set.** The Merry-recognized kfun catalog (`merryapi.c::categorize_merry_word`) lists the kfuns a Merry source may name. The set covers safe arithmetic, string, mapping, array, time, and reflection kfuns: `acos`, `allocate`, `allocate_float`, `allocate_int`, `asin`, `atan`, `atan2`, `call_trace`, `ceil`, `cos`, `cosh`, `crypt`, `ctime`, `error`, `exp`, `explode`, `fabs`, `find_object`, `floor`, `fmod`, `frexp`, `function_object`, `implode`, `ldexp`, `log`, `log10`, `map_indices`, `map_sizeof`, `map_values`, `millitime`, `modf`, `object_name`, `parse_string`, `pow`, `previous_object`, `previous_program`, `random`, `sin`, `sinh`, `sizeof`, `sqrt`, `sscanf`, `status`, `strlen`, `tan`, `tanh`, `this_object`, `time`, `typeof`. Notably absent: every kfun that touches the network (`open_port`, `connect`, `send_datagram`), the file system (`read_file`, `write_file`, `make_dir`, `remove_file`, `rename_file`, `get_dir`, `file_info`), the process (`shutdown`, `dump_state`, `restore_object`, `swapout`), the event surface (`add_event`, `subscribe_event`, `event`, `block_input`), and the code-loading surface (`clone_object`, `compile_object`, `destruct_object`, `call_touch`).

The sandbox (the runtime layer) is the authoritative deny: see [the sandbox surface](#the-sandbox-surface) below for the full list. The categorize list is a tooling hint (used by syntax highlighters and legacy debugging tools); the sandbox does the actual blocking.

## What is added

Four extensions over plain LPC carry Merry's semantics for property-bound scripts:

**`$arg`** — argument reference. Inside a Merry source bound as a script, `$signal` or `$mode` reference values inherited from the invocation context (the LPC caller's `run_merry(ob, signal, mode, args, ...)` populates an `args` mapping that the Merry source sees). The grammar lowers `$arg` to a `VAL_ARGREF` AST node which compiles to a mapping lookup against the inherited `args`.

**`${name}`** — object reference. Resolved at parse time by the `MOB_CONST` rule (`grammar/merry.y` line 262): the parser calls `find_object(name)` immediately, captures the returned object reference into the per-script `obarr`, and emits a `VAL_OBJREF` AST node carrying an index into `obarr`. The compiled LPC reads `obarr[index]` at runtime; the resolution is done once at compile, not per invocation.

**`$delay(seconds, retval, label?)`** — schedules a continuation. The Merry source `$delay(1, FALSE); Set($this, "delay_fired", 1); return TRUE;` evaluates the body synchronously (Set fires immediately), the `$delay` statement returns control to the binding host's `delayed_call` LFUN with a `merry_continuation` callback. `delay` seconds later, `perform_delayed_call` re-enters the same Merry source at a labeled resume point. The host-side glue (`delayed_call` + `perform_delayed_call`) lives in `/lib/util/delayed`, which any script-bearing object inherits. The first call returns the `retval` synchronously; the resume continues from after the `$delay()`.

**`space::method($arg: value, ...)` and `space::`** — cross-namespace invocation. Compiles to a `VAL_LABELCALL` (with explicit method-style args) or `VAL_LABELREF` (without). At runtime, `merrynode::LabelCall` consults the script-space registry on the Merry daemon (registered via `MERRY->register_script_space("space", handler)`), looks up the handler object, and dispatches to its `query_method` / `call_method` LFUN pair. The structured argument form `$who: "world"` lowers to a mapping passed as the second argument to `call_method`. This is the mechanism for runtime-extensible namespaces and external bridges (a future MCP-style integration would register itself as a script space).

The `$this` argument is a useful convention: by default, `run_merry` populates `args["this"]` with the binding host object, so `$this` in Merry source resolves to "the object the script is bound to." Application code may pass additional named arguments via the `args` mapping to `run_merry`.

## Compile pipeline

A Merry source string travels:

1. **Parse** — `/lib/util/parse` and `/lib/util/fileparse` invoke the yacc grammar at `src/usr/Merry/grammar/merry.y` against the source. Output is a tree of mixed arrays whose first element is the `VAL_*` AST tag.
2. **Translate** — `src/usr/Merry/data/merry.c::expand_to_lpc` walks the AST and emits LPC source. The emitted source inherits `/usr/Merry/lib/merrynode` (which installs the sandbox and exposes the merryfun call surface).
3. **Compile** — the Merry daemon (`SYS_MERRY`) computes an md5 hash of the translated LPC, checks an LRU cache (`4*NODE_COUNT` entries), and either reuses a cached `/usr/Merry/merry/<hash>` clonable or calls `compile_object("/usr/Merry/merry/<hash>", translated_source)` to create one.
4. **Clone** — application code calling `new_object(MERRY_DATA, source_string)` invokes the daemon to parse + compile + clone, receiving an LWO suitable for binding to a property via `set_property("merry:<mode>:<signal>", lwo)`.
5. **Invoke** — application LPC calls `run_merry(ob, signal, mode, args)`, which walks `ob`'s UrHierarchy ancestry, finds the bound script via `find_merry`, and calls `script->evaluate(...)`. The evaluate LFUN is generated by the AST-to-LPC translator; it runs the compiled body under the sandbox.

The `examples/merry-app/sys/test.c` driver shows the full pipeline end-to-end across five phases.

## AST node types

The translator and the runtime share an AST tag enumeration in `src/usr/Merry/include/merry.h`:

| Tag | Source construct | Notes |
|---|---|---|
| `VAL_OBJREF` | `${name}` | Indexes into the per-script `obarr` |
| `VAL_ARGREF` | `$arg` | Mapping lookup against inherited `args` |
| `VAL_SLEEP` | `$delay(...)` | Compiles to `delayed_call` invocation |
| `VAL_ARGLIST` | `func(a, b, $key: value, ...)` | Mixed positional + structured assoc args |
| `VAL_PROPGET` | `obj.prop` | Compiles to `Get(obj, "prop")` |
| `VAL_PROPSET` | `obj.prop = expr` | Compiles to `Set(obj, "prop", expr)` |
| `VAL_PROPMOD` | `obj.prop += expr` (and family) | Read-modify-write via `Get` + `Set` |
| `VAL_PROPPOSTFIX` | `obj.prop++` | Compiles to read, then increment, returning original |
| `VAL_PROPPREFIX` | `++obj.prop` | Compiles to increment, then read, returning new |
| `VAL_CONSTANT` | `constant IDENT = expr;` | Compile-time constant reference |
| `VAL_LABELCALL` | `space::method(args)` and `::method(args)` | Resolved at runtime via script-space registry |
| `VAL_LABELREF` | `space::` | Returns the script-space handler object |

There is no `VAL_SAM` tag (the `$"..."` SAM-token surface is a game-content extension of the historical design; eOS-kernellib does not carry it). The numbering above is contiguous.

## The sandbox surface

The sandbox lives in `src/usr/Merry/lib/merrynode.c`. The compiled Merry object inherits `merrynode`, which provides:

- **`call_other` filter** — the local `call_other` shadow (lines 357-370) accepts non-object arguments (calls into pseudo-objects: `nil`, ints, floats, arrays, mappings) and refuses object arguments with `"function 'call_other' not allowed in merry code"`. Cross-object calls must go through `Call(obj, "name", args...)` instead.
- **`new_object` deny** — the local `new_object` shadow (lines 372-375) refuses unconditionally. Cloning new objects is a `Spawn`-merryfun privilege, not a general Merry capability.
- **51-entry kfun deny list** — the `SANDBOX(f)` macro (line 377) defines a function `f` that errors with `"function '<name>' not allowed in merry code"`. Every named kfun is locally shadowed by its sandbox stub. Compiled Merry source that calls one of these names dispatches to the local shadow and raises at runtime.

The full sandbox list, grouped:

- **Network and transport** (14): `open_port`, `ports`, `connect`, `send_datagram`, `send_message`, `connect_datagram`, `datagram_attach`, `datagram_challenge`, `datagram_connect`, `datagram_port`, `datagram_users`, `telnet_connect`, `telnet_port`, `send_close`
- **File system** (10): `read_file`, `write_file`, `make_dir`, `remove_file`, `rename_file`, `remove_dir`, `get_dir`, `file_info`, `dump_file`, `dump_interval`
- **Process and persistence** (5): `shutdown`, `dump_state`, `restore_object`, `swapout`, `execute_program`
- **Code lifecycle** (5): `clone_object`, `compile_object`, `destruct_object`, `destruct_program`, `remove_program`
- **Event surface** (6): `add_event`, `subscribe_event`, `unsubscribe_event`, `event`, `event_except`, `remove_event`
- **Identity and originator** (5): `set_object_name`, `set_originator`, `query_originator`, `this_user`, `users`
- **Other** (6): `block_input`, `call_touch`, `editor`, `query_editor`, `function_object`, `remove_call_out`

The `call_out` kfun is **not** sandboxed (line 388: `/* SANDBOX(call_out) */`). Merry source may schedule deferred work via `call_out`, but the called function still runs under the sandbox.

The merrynode's own merryfuns escape sandbox shadowing using LPC's `::` operator: `::clone_object`, `::destruct_object`, `::new_object`, `::call_other` reach the kfun past the local shadow. The `Spawn` and `Duplicate` merryfuns use `::clone_object` to clone; `Slay` uses `::call_other` to invoke `suicide`. This is the standard escape idiom; Merry source has no `::` escape itself.

## Merryfun call surface

Fifteen merryfuns are available to Merry source. Signatures below are the declarations in `merrynode.c`; the leading type is the return type.

| Merryfun | Signature | Behavior |
|---|---|---|
| `Set` | `mixed Set(object o, string p, mixed v)` | `o->set_property(p, v)`; the wildcard `Set(o, "*", map)` atomically replaces the entire property map. Errors if `o` is nil |
| `Get` | `mixed Get(object o, string p)` | `o->query_property(p)`; the wildcard `Get(o, "*")` returns the full property map. Errors if `o` is nil |
| `BatchedSet` | `mixed BatchedSet(object o, mapping kv_map, varargs mapping opts)` | Multi-key property write via `MERRY->batched_set(o, kv_map[, opts])`; pass `([ "atomic": 1 ])` as `opts` to opt into atomic mode (absent or nil runs non-atomic). The function-reference batch form (`MERRY->batch(fn)`) is not exposed — Merry has no function-reference syntax |
| `SetVar` | `mixed SetVar(string n, mixed v)` | Writes `v` into the inherited `args` mapping under `lower_case(n)` |
| `GetVar` | `mixed GetVar(string n)` | Reads `args[lower_case(n)]` from the inherited `args` mapping |
| `Call` | `mixed Call(mixed oref, string name, varargs mixed *local)` | Cross-object call (replacement for `->`); resolves `oref` (object or path string), then dispatches `obj->call_method(name, args)` if defined, else `run_merry(obj, name, "lib", args)`. `local` is key-value pairs spliced into the callee's `args`. Errors if neither method nor script exists |
| `LabelCall` | `mixed LabelCall(string space, string fun, varargs mixed *local)` | Looks up the handler registered for script-space `space` via the Merry daemon's registry, then delegates to `Call(handler, fun, local)`. Errors if the space is not registered |
| `LabelRef` | `object LabelRef(string space)` | Returns the script-space's registered handler object without calling it |
| `FindMerry` | `object FindMerry(mixed oref, string mode, string signal)` | Walks `oref`'s Ur-hierarchy and returns the ancestor holding the named `merry:<mode>:<signal>` script |
| `Spawn` | `object Spawn(object ur)` | Clones the ur's clonable (via the `::clone_object` escape) and stamps the result with `ur` as parent. Atomic |
| `Slay` | `void Slay(object obj)` | Calls `obj->suicide()` to schedule destruction. Errors if `obj` is nil |
| `Duplicate` | `object Duplicate(object ob)` | Clones `ob`'s program and re-imports its full state via Schema marshaling. Atomic; errors if `ob` is nil or not a clone |
| `In` | `string In(string signal, int seconds)` | Schedules a single re-fire of `signal` on `this` at `t+seconds` via `schedule_entry`; returns the schedule-entry id |
| `Every` | `string Every(string signal, int seconds)` | Schedules a recurring re-fire of `signal` every `seconds`; returns the schedule-entry id |
| `Stop` | `string Stop(string id)` | Cancels a scheduled `In` or `Every` entry by id via `unschedule_entry` |

For raising errors from inside Merry source, use the allowed kfun `error("message")` directly — there is no `Error` merryfun. (The `categorize_merry_word` syntax-categorizer in `merryapi.c` lists `Error` as a merryfun token, but the merrynode implementation carries no `Error` LFUN; the entry is vestigial categorization tooling.)

All fifteen merryfuns are marked `nomask`: subclasses cannot redefine them. `Spawn` and `Duplicate` are also `atomic`.

The `Set(obj, "*", mapping)` and `Get(obj, "*")` forms are bulk: the wildcard `"*"` replaces or returns the entire property map atomically (`merrynode.c::set_all` clears then re-sets under one atomic block).

## A short worked example

The reference application (`examples/merry-app/`) binds five Merry sources during boot. The simplest is the ancestry-walk assertion:

```lpc
/* LPC side: bind a Merry source to the parent's "lib:greet" property */
script = new_object(MERRY_DATA,
                    "return \"MerryApp:test: ANCESTRY OK\";");
parent->set_property("merry:lib:greet", script);

/* Invoke from the child; find_merry walks the ur chain and finds the
 * script on the parent, runs it, returns the string. */
result = run_merry(child, "greet", "lib", ([ ]));
/* result == "MerryApp:test: ANCESTRY OK" */
```

A sandbox-firing assertion:

```lpc
/* LPC side: compile and bind a Merry source that calls a sandboxed kfun. */
bad_script = new_object(MERRY_DATA, "clone_object(\"/foo/bar\");");
parent->set_property("merry:lib:badcall", bad_script);

catch {
    run_merry(child, "badcall", "lib", ([ ]));
} : {
    /* The sandbox raised; the catch absorbs it. */
}
```

A `$delay()` continuation:

```lpc
/* LPC side: bind a Merry source that uses $delay to schedule a continuation. */
delay_script = new_object(MERRY_DATA,
                          "$delay(1, FALSE); " +
                          "Set($this, \"delay_fired\", 1); " +
                          "return TRUE;");
parent->set_property("merry:lib:delay_test", delay_script);
result = run_merry(child, "delay_test", "lib", ([ ]));
/* result == FALSE (the $delay returns FALSE synchronously)
 * One second later, $this's "delay_fired" property is set to 1. */
```

A `space::method()` cross-script-space dispatch:

```lpc
/* LPC side: register self as the "testspace" handler. */
MERRY_DAEMON->register_script_space("testspace", this_object());

label_script = new_object(MERRY_DATA,
                          "return testspace::greet($who: \"world\");");
parent->set_property("merry:lib:label_test", label_script);
result = run_merry(child, "label_test", "lib", ([ ]));
/* result == "Hello world", returned by this object's call_method LFUN */
```

The LFUNs that make these examples work split by home: the binding host's `delayed_call` and `perform_delayed_call` pair comes from `/lib/util/delayed`, while the handler's `query_method` and `call_method` pair lives alongside the Merry source in `examples/merry-app/`. See [merry-applications.md](merry-applications.md) for the application-author walkthrough of the same example.

## Where to next

- **[merry-applications.md](merry-applications.md)** — the application-author perspective: writing a script-bearing object, the `merry:<mode>:<signal>` storage convention, the ancestry walk via `find_merry`, the LPC-side invocation surface.
- **[lpc-essentials.md](lpc-essentials.md)** — LPC language orientation. Read this if `inherit`, `clone_object`, `call_out`, `atomic`, or the per-call atomicity model are unfamiliar.
- **[runtime-primitives.md](runtime-primitives.md)** — the sandboxed-code-load primitive that Merry provides, and how it composes with the other runtime primitives.
- **`src/usr/Merry/grammar/merry.y`** — the authoritative grammar. Read this when a parse error is unexplained or when extending the language.
- **`src/usr/Merry/lib/merrynode.c`** — the sandbox installer and merryfun implementations. Read this when a runtime error from inside a Merry script needs tracing.
- **`src/usr/Merry/lib/merryapi.c`** — the `find_merry` / `run_merry` / `find_merries` / `run_merries` invocation API used by LPC code that calls into bound scripts.
- **`examples/merry-app/`** — the five-phase reference application that exercises the ancestry walk, sandbox firing, Spawn, $delay, and LabelCall.
