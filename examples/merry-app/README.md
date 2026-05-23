# Reference Merry application

A minimal Merry application that runs on top of eOS-kernellib. Demonstrates the platform contracts a property-bearing object honors when it participates in Merry's sublanguage-runtime invocation surface: the property store, the ur-hierarchy ancestry walk, the find_merry / run_merry script-lookup convention, the `Spawn` merryfun, the `$delay()` continuation path, and the LabelCall / LabelRef script-space dispatch. The full walkthrough lives in `docs/merry-applications.md`.

## Operations

- A `MerryApp:Thing` clonable inherits `/lib/util/properties` (storage for `merry:<mode>:<signal>` keys), `/lib/util/ur` (ur-parent + ur-child tracking), and `/lib/util/named` (logical-name registration via Index). It also exposes a `delayed_call` / `perform_delayed_call` pair so Merry's `$delay()` continuation can schedule against it.
- The boot-time test driver builds a 2-generation Hierarchy: a parent clone and a child clone whose ur-parent is the parent. A Merry script returning a literal string is bound to the parent under `merry:lib:greet`.
- `MERRY->run_merry(child, "greet", "lib", ([]))` walks the child's ancestry via `query_ur_object()`, finds the script on the parent, evaluates it, and returns the literal.
- A second Merry script attempts `clone_object("/foo/bar")` from inside Merry source. The SANDBOX(f) deny list in `merrynode.c` shadows `clone_object` with an error-throwing local; the call surfaces as "function 'clone_object' not allowed in merry code" and the test driver logs the sandbox firing.
- A third Merry script calls `Spawn($this)` on the binding host. `Spawn` uses `::clone_object` to escape the local SANDBOX shadow, clones the binding host's clonable, and sets the new clone's ur-object to the binding host. The test verifies the spawned name matches the binding host's clonable path.
- A fourth Merry script issues `$delay(1, FALSE); Set($this, "delay_fired", 1);`. The first call schedules a continuation via the binding host's `delayed_call`; one second later, `perform_delayed_call` fires `merry_continuation` on the mcontext LWO, which calls `run_merries` to resume execution and set the marker property. A `verify_delay` call_out polls the property at t=+2.
- A fifth Merry script invokes `testspace::greet($who: "world");`. The test driver registers itself as the "testspace" script-space handler (exposing `query_method` / `call_method`); `LabelCall` walks the registry, finds the handler, and dispatches the named method with the inline locals overlaid on the merry-source `args` mapping.
- The DI-2 batching surface adds four more phases. Phase 6 calls `MERRY->batched_set(child, ([ "k1": 100, "k2": 200 ]))` from LPC and verifies both values land. Phase 7 exercises the DD-4 (d) atomic-mode opt-in via `MERRY->batched_set(child, mapping, ([ "atomic": 1 ]))`. Phase 8 runs `BatchedSet($this, ([ ... ]))` from a Merry-compiled script to confirm the merrynode.c surface is reachable from observer source. Phase 9 invokes `MERRY->batch(this_object(), "_throw_for_test", ({}))` against a callable that errors and verifies the catch'd-error default per DD-2 (d) propagates the error AND that the daemon-local batch-status stub records a `main-aborted` entry per DD-3 (c).
- The DI-3 dispatcher adds six more phases against the same `child` host. Phase 10 (DISPATCH MAIN) registers a main-timing observer and confirms it fires on the next `set_property`. Phase 11 (DISPATCH ORDER) registers pre + main + post observers that append to a trace string, asserting `pre|main|post` after the write per DD-4 (b). Phase 12 (DISPATCH VETO) registers a pre observer that throws and verifies the propagated error AND that the underlying value did not land (DD-4 (a) pre-veto contract). Phase 13 (DISPATCH CYCLE) registers a main observer that writes the same property and verifies the cycle detector (DD-2 (b) per-execution chain) catches the recursive dispatch. Phase 14 (DISPATCH ANCESTRY) registers an observer on the parent and writes the property on the child; find_observers walks the ur chain and fires the parent's observer with `$this` bound to the child host (DD-5 (a)). Phase 15 (DISPATCH IMPLICIT) confirms an unbatched `set_property` enters and exits an implicit batch cleanly and records a `completed` status entry (DD-3 (b)).

## Deployment

Copy the directory into the kernel layer's `src/usr/` (`MerryApp` is the example's choice; pick any `/usr/<Name>/` that doesn't conflict with an existing domain):

```sh
cp -R examples/merry-app src/usr/MerryApp
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. The cross-domain clone of `~Merry/data/merry` requires Merry to be globally readable -- granted in `src/usr/System/initd.c`.

## Verify

```sh
rm -f .runtime/state/snapshot .runtime/state/snapshot.old
scripts/setup-runtime.sh
.runtime/bin/dgd mva.dgd &
sleep 5
cat .runtime/src/usr/MerryApp/data/test-result.log
# Expect (in order; DELAY OK arrives ~2 seconds after the others):
#   MerryApp:test: starting
#   MerryApp:test: ANCESTRY OK
#   MerryApp:test: SANDBOX OK
#   MerryApp:test: SPAWN OK
#   MerryApp:test: LABELCALL OK
#   MerryApp:test: BATCH OK
#   MerryApp:test: BATCH ATOMIC OK
#   MerryApp:test: BATCH SOURCE OK
#   MerryApp:test: BATCH ABORT OK
#   MerryApp:test: DISPATCH MAIN OK
#   MerryApp:test: DISPATCH ORDER OK
#   MerryApp:test: DISPATCH VETO OK
#   MerryApp:test: DISPATCH CYCLE OK
#   MerryApp:test: DISPATCH ANCESTRY OK
#   MerryApp:test: DISPATCH IMPLICIT OK
#   MerryApp:test: DELAY OK
```

The sentinel file lives at the DGD-internal path `/usr/MerryApp/data/test-result.log`, which lands at `<directory>/usr/MerryApp/data/test-result.log` on the host filesystem (`<directory>` is the DGD config's `directory` setting; `.runtime/src/` in the standard layout).

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot.
- `lib/app.c` -- marker lib paralleling `examples/vault-app/lib/app.c`. Daemons under `sys/` inherit it so future shared daemon-side state has a place to land.
- `obj/thing.c` -- property + ur-bearing clonable. Inherits `/lib/util/properties`, `/lib/util/ur`, `/lib/util/named` with labeled inherits to disambiguate the shared `create()` between properties and ur. Also defines the `delayed_call` / `perform_delayed_call` pair every script-bearing object must expose for Merry's `$delay()` to schedule continuations.
- `sys/test.c` -- boot-time test driver; clones two things, ties them via `set_ur_object`, registers Merry scripts for phases 1-3+5+8, runs eight synchronous assertions via `call_out("setup_and_run", 0)`, and one asynchronous `$delay()` assertion verified via a second `call_out` at t=+2. Doubles as the "testspace" script-space handler for phase 5 and exposes `_throw_for_test` as the callable phase 9's `MERRY->batch()` exercises against the catch'd-error abort path; in production both auxiliary roles would typically live on distinct objects.

## Notes

- The example exercises invocation only -- no Schema registration, no on-disk Vault round-trip. A Merry script is a runtime object (a `/usr/Merry/data/merry` clone) carrying parsed AST + cached compiled program; statedump survival of bound scripts requires Schema + Marshal participation that this example deliberately omits to keep the test focused on the runtime path.
- `Duplicate` shares the `::clone_object` escape with `Spawn` (both fixed in merrynode at LM-6) but `export_state` requires a Schema-registered state model (`SID->get_root_node(ob)`). A Schema registration on `MerryApp:Thing` would unblock Duplicate; that work pairs with whichever phase first introduces Schema-bound Merry hosts.
- Observer-property naming (e.g., `merry:on:<property-path>[:<timing>]` storage for property-change observers) is deferred to the DD phase per the LM-5 Decision. The runtime mechanism find_merry uses (ancestry walk + prefix fan-out + `merry:inherit:` delegation) is unchanged; only the storage-key convention is unsettled.
