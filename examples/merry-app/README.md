# Reference Merry application

A minimal Merry application that runs on top of eOS-kernellib. Demonstrates the platform contracts a property-bearing object honors when it participates in Merry's sublanguage-runtime invocation surface: the property store, the ur-hierarchy ancestry walk, the find_merry / run_merry script-lookup convention, the `Spawn` merryfun, the `$delay()` continuation path, and the LabelCall / LabelRef script-space dispatch. The full walkthrough lives in `docs/merry-applications.md`.

## Operations

- A `MerryApp:Thing` clonable inherits `/lib/util/properties` (storage for `merry:<mode>:<signal>` keys), `/lib/util/ur` (ur-parent + ur-child tracking), `/lib/util/named` (logical-name registration via Index), and `/lib/util/delayed` (the `delayed_call` / `perform_delayed_call` pair so Merry's `$delay()` continuation can schedule against it).
- The boot-time test driver builds a 2-generation Hierarchy: a parent clone and a child clone whose ur-parent is the parent. A Merry script returning a literal string is bound to the parent under `merry:lib:greet`.
- `MERRY->run_merry(child, "greet", "lib", ([]))` walks the child's ancestry via `query_parent()`, finds the script on the parent, evaluates it, and returns the literal.
- A second Merry script attempts `clone_object("/foo/bar")` from inside Merry source. The SANDBOX(f) deny list in `merrynode.c` shadows `clone_object` with an error-throwing local; the call surfaces as "function 'clone_object' not allowed in merry code" and the test driver logs the sandbox firing.
- A third Merry script calls `Spawn($this)` on the binding host. `Spawn` uses `::clone_object` to escape the local SANDBOX shadow, clones the binding host's clonable, and sets the new clone's ur-object to the binding host. The test verifies the spawned name matches the binding host's clonable path.
- A fourth Merry script issues `$delay(1, FALSE); Set($this, "delay_fired", 1);`. The first call schedules a continuation via the binding host's `delayed_call`; one second later, `perform_delayed_call` fires `merry_continuation` on the mcontext LWO, which calls `run_merries` to resume execution and set the marker property. A `verify_delay` call_out polls the property at t=+2.
- A fifth Merry script invokes `testspace::greet($who: "world");`. The test driver registers itself as the "testspace" script-space handler (exposing `query_method` / `call_method`); `LabelCall` walks the registry, finds the handler, and dispatches the named method with the inline locals overlaid on the merry-source `args` mapping.
- The batching surface adds four more phases. Phase 6 calls `MERRY->batched_set(child, ([ "k1": 100, "k2": 200 ]))` from LPC and verifies both values land. Phase 7 exercises the atomic-mode opt-in via `MERRY->batched_set(child, mapping, ([ "atomic": 1 ]))`. Phase 8 runs `BatchedSet($this, ([ ... ]))` from a Merry-compiled script to confirm the merrynode.c surface is reachable from observer source. Phase 9 invokes `MERRY->batch(this_object(), "_throw_for_test", ({}))` against a callable that errors and verifies the catch'd-error default propagates the error AND that the daemon-local batch-status stub records a `main-aborted` entry per the batch-status contract.
- The dispatcher adds six more phases against the same `child` host. Phase 10 (DISPATCH MAIN) registers a main-timing observer and confirms it fires on the next `set_property`. Phase 10b (DISPATCH FANOUT) registers a second observer under the same `merry:on:<path>:<timing>` key -- the property stores a list -- and confirms one write fires both, in registration order. Phase 11 (DISPATCH ORDER) registers pre + main + post observers that append to a trace string, asserting `pre|main|post` after the write per the pre->write->main->post ordering contract. Phase 12 (DISPATCH VETO) registers a pre observer that throws and verifies the propagated error AND that the underlying value did not land (the pre-veto contract). Phase 13 (DISPATCH CYCLE) registers a main observer that writes the same property and verifies the cycle detector (the cascade-bound design per-execution chain) catches the recursive dispatch. Phase 14 (DISPATCH ANCESTRY) registers an observer on the parent and writes the property on the child; find_observers walks the ur chain and fires the parent's observer with `$this` bound to the child host (the ancestry-walk design). Phase 15 (DISPATCH IMPLICIT) confirms an unbatched `set_property` enters and exits an implicit batch cleanly and records a `completed` status entry (the implicit-batch semantics).
- The observer formalization adds five phases. Phase 15b (OBSERVER QUERY) exercises the daemon's three public read-only views: `query_observers` (the local slot, in registration order), `query_effective_observers` (the ancestry-walk view, each entry labeled with the owning ancestor -- the parent's phase-14 observer is visible from the child), and `query_observed_paths` (enumeration of the object's observed `(path, timing)` slots). Phase 15c (OBSERVER SUGAR) registers one source under an ARRAY of paths and confirms the source compiled once (both slots hold the same compiled object) and fires once per observed-path write. Phase 15d (OBSERVER REMOVE) removes the first fanout observer by index via `remove_observer`, confirms the second still fires alone, and asserts two refusals: an out-of-range index throws, and a cross-domain caller is refused by the registrar gate. Phase 15e (OBSERVER EVICT) registers an observer, destructs its compiled `/usr/Merry/merry/<md5>` program via the LRU-eviction entry point (`suicide()`), and -- post-restore -- confirms the observer still fires: the stored slot entry is the `data/merry` wrapper, which retains the source and lazily recompiles the program on the next evaluate. Phase 15f (OBSERVER CLEAR) registers two observers on a fresh path, clears the triple with the coarse `unregister_observer`, and confirms the slot property is deleted and a subsequent write fires nothing -- the coarse clear's success path beside phase 15d's surgical removal.
- Phases 16 and 17 verify observer-state survival across a snapshot + restore cycle. Phase 16 (PERSIST SETUP) clones a fresh parent/child pair, registers a main observer on the child for `test:persist:val`, saves the host as a `static` global on the test driver, schedules a `phase17_verify` `call_out`, then calls a System-tier helper to `dump_state(FALSE)` and `shutdown()`. The verify script restarts DGD against the snapshot. Phase 17 (PERSIST VERIFY) fires from the surviving `call_out` after restore: writes `42` to the observed path on the restored host and asserts both that the write landed (property storage survived) and that the observer fired (the compiled merry-script object reference survived and `_resolve_observer` rebound correctly). The cycle exercises five orthogonal-persistence guarantees end-to-end: LPC global variables, property storage, object references to per-app data clones (`/usr/Merry/merry/<md5>`), the dispatcher's observer-source contract, and the scheduled-`call_out` queue.

## Deployment

Copy the directory into the kernel layer's `src/usr/` (`MerryApp` is the example's choice; pick any `/usr/<Name>/` that doesn't conflict with an existing domain):

```sh
cp -R examples/merry-app src/usr/MerryApp
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. The cross-domain clone of `~Merry/data/merry` requires Merry to be globally readable -- granted in `src/usr/System/initd.c`.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh merry-app
```

The runner does the clean-slate deploy, drives the boot cycle, and
asserts the sentinel count; boot output lands under `state/`. The
manual sequence it automates:

```sh
# Clean slate: remove any prior deploy and state, then redeploy.
rm -rf src/usr/MerryApp
rm -f state/snapshot state/snapshot.old
cp -R examples/merry-app src/usr/MerryApp

# Cold boot: phases 1-16 run; phase 16 dumps a snapshot and the driver exits.
/path/to/dgd/bin/dgd example.dgd
# (driver exits on its own after PERSIST SETUP OK)

# Restore: restart against the snapshot; phase 17 fires from the surviving
# call_out and writes PERSIST VERIFY OK.
/path/to/dgd/bin/dgd example.dgd state/snapshot &
sleep 5
cat src/usr/MerryApp/data/test-result.log
kill %1
# Expect (in order; SUICIDE OK, DELAY OK, OBSERVER EVICT OK and
# PERSIST VERIFY OK land on the second boot from pre-snapshot call_outs
# whose scheduled times have elapsed, in their scheduling order: phase
# 3c's verify_suicide, then phase 4's verify_delay, then phase 15e's
# verify_evict, then phase 16's phase17_verify):
#   MerryApp:test: starting
#   MerryApp:test: ANCESTRY OK
#   MerryApp:test: SANDBOX OK
#   MerryApp:test: SPAWN OK
#   MerryApp:test: DUPLICATE OK
#   MerryApp:test: LABELCALL OK
#   MerryApp:test: REGISTRAR REJECT OK
#   MerryApp:test: TARGET REJECT OK
#   MerryApp:test: DISPATCH GATE OK
#   MerryApp:test: BATCH OK
#   MerryApp:test: BATCH ATOMIC OK
#   MerryApp:test: BATCH SOURCE OK
#   MerryApp:test: BATCH ABORT OK
#   MerryApp:test: DISPATCH MAIN OK
#   MerryApp:test: DISPATCH FANOUT OK
#   MerryApp:test: DISPATCH ORDER OK
#   MerryApp:test: DISPATCH VETO OK
#   MerryApp:test: DISPATCH CYCLE OK
#   MerryApp:test: DISPATCH ANCESTRY OK
#   MerryApp:test: DISPATCH IMPLICIT OK
#   MerryApp:test: OBSERVER QUERY OK
#   MerryApp:test: OBSERVER SUGAR OK
#   MerryApp:test: OBSERVER REMOVE OK
#   MerryApp:test: OBSERVER CLEAR OK
#   MerryApp:test: PERSIST SETUP OK
#   --- (driver exits; restart against snapshot) ---
#   MerryApp:test: SUICIDE OK
#   MerryApp:test: DELAY OK
#   MerryApp:test: OBSERVER EVICT OK
#   MerryApp:test: PERSIST VERIFY OK
```

The sentinel file lives at the DGD-internal path `/usr/MerryApp/data/test-result.log`, which lands at `<directory>/usr/MerryApp/data/test-result.log` on the host filesystem (`<directory>` is the DGD config's `directory` setting; this repository's `src/` in the example configuration).

## Phase notes

The per-phase walkthrough for the five assertions summarized in `../../docs/merry-applications.md` "Verification -- the merry-app sentinels". Read the code in `sys/test.c` alongside this section.

### Phase 1 -- ancestry walk

```c
parent = clone_object(THING_PROG);
parent->set_object_name("MerryApp:demo:parent");

child = clone_object(THING_PROG);
child->set_object_name("MerryApp:demo:child");
child->set_ur_object(parent);

script = new_object(MERRY_DATA,
                    "return \"MerryApp:test: ANCESTRY OK\";");
parent->set_property("merry:lib:greet", script);

result = run_merry(child, "greet", "lib", ([ ]));
```

`run_merry(child, ...)` calls `find_merry(child, "greet", "lib")` which:

1. queries `child` for `merry:lib:greet` -- nil (script was bound on parent);
2. queries `child` for `merry:inherit:lib:greet` -- nil;
3. walks to `child->query_parent()` -> parent;
4. queries `parent` for `merry:lib:greet` -- finds the script object;
5. returns the script.

`run_merry` then calls `script->evaluate(child, "greet", "lib", ([ ]), nil, "MerryApp:demo:child/lib:greet")`. The clonable's evaluate forwards to the compiled `/merry/<md5>` program's evaluate, which sets up `this = child`, `args = ([])`, then calls `merry("greet", "lib", "virgin")`. The wrapped switch on `label == "virgin"` fires the body's `return "MerryApp:test: ANCESTRY OK"`. The string propagates back up the call stack.

### Phase 2 -- sandbox firing

```c
bad_script = new_object(MERRY_DATA,
                        "clone_object(\"/foo/bar\");");
parent->set_property("merry:lib:badcall", bad_script);

catch {
    run_merry(child, "badcall", "lib", ([ ]));
    /* unreachable on success */
} : {
    log_line("MerryApp:test: SANDBOX OK");
}
```

The Merry source `clone_object("/foo/bar");` parses cleanly -- the parser accepts the identifier-call syntax. At evaluation, the wrapped LPC calls `clone_object("/foo/bar")` against the compiled program's namespace; `merrynode.c`'s `SANDBOX(clone_object)` macro defines a local error-thrower that takes precedence over the kfun. The call raises `function 'clone_object' not allowed in merry code`, which the test driver's `catch` block captures and logs as the success sentinel.

### Phase 3 -- Spawn merryfun

```c
spawn_script = new_object(MERRY_DATA,
                          "return object_name(Spawn($this));");
parent->set_property("merry:lib:make_one", spawn_script);
result = run_merry(child, "make_one", "lib", ([ ]));
/* result is "/usr/MerryApp/obj/thing#NN" */
```

`Spawn($this)` resolves to the static `Spawn(object ur)` merryfun on `merrynode`, which extracts the binding host's clonable from its object_name and calls `::clone_object(clonable)` -- the `::` escape past the local `SANDBOX(clone_object)` shadow (`../../docs/merry-applications.md` "What the sandbox forbids" walks the escape idiom). Phase 3b registers the `MerryApp:Thing` schema (one property-backed `label` attribute) at setup, then asserts a Merry-invoked `Duplicate($this)` returns a distinct clone of the same clonable carrying the schema-declared state (`export_state` resolves the model via `SID->get_root_node(ob)`; `import_state` replays it on the fresh clone).

### Phase 4 -- $delay() and the 5-arg mcontext dispatch

```c
delay_script = new_object(MERRY_DATA,
                          "$delay(1, FALSE); " +
                          "Set($this, \"delay_fired\", 1); " +
                          "return TRUE;");
parent->set_property("merry:lib:delay_test", delay_script);
run_merry(child, "delay_test", "lib", ([ ]));
/* schedules verify_delay call_out at t=+2 sec */
```

The grammar expands `$delay(1, FALSE)` to `{ do_delay(mode, signal, 1, "<label>"); return FALSE; case "<label>": ; }`. `do_delay` calls `::call_other(this, "delayed_call", new_object("/usr/Merry/data/mcontext", signal, mode, label, args, this_object()), "merry_continuation", 1, this)`. The binding host's `delayed_call` LFUN (inherited from `/lib/util/delayed`) wraps that in `call_out("perform_delayed_call", 1, mcontext, "merry_continuation", ({ this }))`. After one second, `perform_delayed_call` fires `merry_continuation` on the mcontext LWO, which calls `run_merries(this, signal, mode, args, label)` -- the second pass resumes execution at the case label and the `Set($this, "delay_fired", 1)` line runs. A `verify_delay` call_out polls `query_raw_property("delay_fired")` at t=+2 and logs the sentinel.

The path validates cloud-server's `new_object(path, args...)` -> `_F_init` -> `create(args...)` 5-arg dispatch (mcontext's `create(string m, string s, string l, mapping a, varargs object c)`, where the fifth, varargs `c` carries the running compiled merry object so a resumed continuation -- including a dispatcher-fired observer's -- can call back into it directly).

### Phase 5 -- LabelCall and the script-space handler protocol

```c
MERRY->register_script_space("testspace", this_object());
/* this_object() exposes:
 *   int   query_method(string name);
 *   mixed call_method(string name, mapping args); */

label_script = new_object(MERRY_DATA,
                          "return testspace::greet($who: \"world\");");
parent->set_property("merry:lib:label_test", label_script);
result = run_merry(child, "label_test", "lib", ([ ]));
/* result is "Hello world" */
```

The grammar maps `testspace::greet($who: "world")` to `LabelCall("testspace", "greet", ({ "who", "world" }))`. `merrynode.c::LabelCall` queries `MERRY->query_script_space("testspace")`, then calls `Call(handler, "greet", local)`. `Call` walks the handler's `query_method` -- a non-zero return routes through `obj->call_method(name, args)`, where `args` is the merry-source `args` mapping with the inline locals overlaid. The handler reads `args["who"]` and returns `"Hello world"`.

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot.
- `lib/app.c` -- marker lib paralleling `examples/vault-app/lib/app.c`. Daemons under `sys/` inherit it so future shared daemon-side state has a place to land.
- `obj/thing.c` -- property + ur-bearing clonable. Inherits `/lib/util/properties`, `/lib/util/ur`, `/lib/util/named` with labeled inherits to disambiguate the shared `create()` between properties and ur. Also inherits `/lib/util/delayed` for the `delayed_call` / `perform_delayed_call` pair every script-bearing object must expose for Merry's `$delay()` to schedule continuations.
- `sys/test.c` -- boot-time test driver; clones two things, ties them via `set_ur_object`, registers Merry scripts for phases 1-3+5+8, runs the synchronous assertions via `call_out("setup_and_run", 0)`, and the `$delay()` assertion verified via a second `call_out` at t=+2. Phase 16 schedules `phase17_verify` and dumps a snapshot via `/usr/System/sys/persist_helper`; phase 17 fires from the surviving `call_out` after the snapshot restore and verifies observer-state survival. Doubles as the "testspace" script-space handler for phase 5 and exposes `_throw_for_test` as the callable phase 9's `MERRY->batch()` exercises against the catch'd-error abort path; in production both auxiliary roles would typically live on distinct objects.

## Notes

- The example exercises invocation and observer-survival across snapshot+restore -- no Schema registration, no on-disk Vault round-trip. A Merry script is a runtime object (a `/usr/Merry/data/merry` clone) carrying parsed AST + cached compiled program; the cached compile artifact lives on-disk under `/usr/Merry/merry/<md5>` and DGD's object-reference resurrection re-attaches the property's stored reference on restore. Phases 16 and 17 verify this end-to-end at the dispatcher layer; Schema + Marshal round-trips are out of scope and demonstrated by `examples/vault-app/`.
- `Duplicate` shares the `::clone_object` escape with `Spawn`, and additionally walks the source clone's registered state model (`export_state` resolves it via the schema daemon). Phase 3b registers the `MerryApp:Thing` schema (one property-backed `label` attribute) and asserts a Merry-invoked `Duplicate($this)` returns a distinct clone carrying the schema-declared state.
- Observer-property naming (`merry:on:<path>:<timing>` storage for property-change observers) is the property-change dispatcher's convention; phases 10-15 exercise registration and pre/main/post firing, and phases 16-17 exercise survival across snapshot+restore. The convention and its lookup semantics (ancestry walk via `query_parent()`, declarative-dominant with `merry:on-inherit:<path>:<timing>` re-enable marker, observer-source `$this` binding) are documented in `docs/dispatcher.md`.
