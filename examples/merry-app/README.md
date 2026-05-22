# Reference Merry application

A minimal Merry application that runs on top of eOS-kernellib. Demonstrates the platform contracts a property-bearing object honors when it participates in Merry's sublanguage-runtime invocation surface: the property store, the ur-hierarchy ancestry walk, and the find_merry / run_merry script-lookup convention. The full walkthrough lives in `docs/merry-applications.md`.

## Operations

- A `MerryApp:Thing` clonable inherits `/lib/util/properties` (storage for `merry:<mode>:<signal>` keys), `/lib/util/ur` (ur-parent + ur-child tracking), and `/lib/util/named` (logical-name registration via Index).
- The boot-time test driver builds a 2-generation Hierarchy: a parent clone and a child clone whose ur-parent is the parent. A Merry script returning a literal string is bound to the parent under `merry:lib:greet`.
- `MERRY->run_merry(child, "greet", "lib", ([]))` walks the child's ancestry via `query_ur_object()`, finds the script on the parent, evaluates it, and returns the literal.
- A second Merry script attempts `clone_object("/foo/bar")` from inside Merry source. The SANDBOX(f) deny list in `merrynode.c` shadows `clone_object` with an error-throwing local; the call surfaces as "function 'clone_object' not allowed in merry code" and the test driver logs the sandbox firing.

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
# Expect (in order):
#   MerryApp:test: starting
#   MerryApp:test: ANCESTRY OK
#   MerryApp:test: SANDBOX OK
```

The sentinel file lives at the DGD-internal path `/usr/MerryApp/data/test-result.log`, which lands at `<directory>/usr/MerryApp/data/test-result.log` on the host filesystem (`<directory>` is the DGD config's `directory` setting; `.runtime/src/` in the standard layout).

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot.
- `lib/app.c` -- marker lib paralleling `examples/vault-app/lib/app.c`. Daemons under `sys/` inherit it so future shared daemon-side state has a place to land.
- `obj/thing.c` -- property + ur-bearing clonable. Inherits `/lib/util/properties`, `/lib/util/ur`, `/lib/util/named` with labeled inherits to disambiguate the shared `create()` between properties and ur.
- `sys/test.c` -- boot-time test driver; clones two things, ties them via `set_ur_object`, registers two Merry scripts, runs both assertions via `call_out("setup_and_run", 0)`.

## Notes

- The example exercises invocation only -- no Schema registration, no on-disk Vault round-trip. A Merry script is a runtime object (a `/usr/Merry/data/merry` clone) carrying parsed AST + cached compiled program; statedump survival of bound scripts requires Schema + Marshal participation that this example deliberately omits to keep the test focused on the runtime path.
- `Duplicate` (merrynode's deep-clone via `export_state` + `clone_object` + `import_state`) and `Spawn` (merrynode's clone + `set_ur_object`) both call `clone_object` from inside merrynode -- the same SANDBOX shadow that this example's phase 2 demonstrates. They will need an `::clone_object` escape (or a separately-inherited library) before a Merry source can use them. That fix is L8 cascade work paired with a richer test in LM-6.
- `$delay()` (`do_delay` -> `mcontext.c::create(4-arg)`) is not exercised here. The wrapper is in place; first exercise will validate cloud-server's `new_object(path, args...)` -> `_F_init` -> `create(args...)` dispatch for the 4-arg case.
- LabelCall / LabelRef (script-space resolution via `space::method()` syntax) is not exercised here. A separate test driver that registers a script-space handler with `MERRY->register_script_space` will cover it.
