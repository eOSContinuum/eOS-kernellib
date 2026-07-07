# Upgrade-cascade demonstration

**Audience**: application authors and operators wanting empirical evidence for the library-inheritance half of the hot-reload primitive — upgrading a parent library through the upgrade daemon recompiles its inheritors and `call_touch`-patches their existing clones, while each clone keeps its own state (`docs/runtime-primitives.md` section 4, `docs/code-lifecycle.md`).

A minimal headless example. `lib/shape` is a parent library; `obj/widget` is a clonable master that inherits it. The boot-time driver `sys/test` clones two widgets, advances them to different counts, stages new library source on disk (`shape_version()` "v2", `scale()` times 3), and drives the upgrade daemon (`/usr/System/sys/upgraded`) through the System auto library's `upgrade()` wrapper, atomically, with itself as the patchtool. From the next dispatch it confirms the existing clones run the upgraded library (`CASCADE PROPAGATE`, `LIB BEHAVIOR`), keep their counts (`CLONE STATE SURVIVED`), and had `patch()` run exactly once each through the `call_touch` gate (`CLONE PATCH`).

This complements `hot-reload-master`, which recompiles a single clonable master in place. Here the recompiled unit is a *library*: the upgrade daemon walks the dependency graph the object manager (`/usr/System/sys/objectd.c`) recorded at compile time, destructs the old library issue, recompiles the dependent master, and visits each existing clone so its `patch()` hook can migrate state — the mechanism behind the operator `upgrade -p` command, driven here programmatically.

## Layout

- `lib/shape.c` — parent library: `shape_version()`, `scale()`. The upgraded unit; its source is replaced live by the driver.
- `obj/widget.c` — clonable inheritor: per-clone `count`, `describe()` composed with the library, and the `patch()` hook the `call_touch` gate reaches. Clonable objects live under `obj/` (the kernel's `clone_object` only accepts paths containing `/obj/`).
- `sys/test.c` — boot-time driver and patchtool; writes sentinels to `data/test-result.log`.
- `initd.c` — compiles the widget (pulling in the library) and the driver at boot.

## Deployment

Copy the directory into the kernel layer's `src/usr/` as the `Cascade` domain:

```sh
cp -R examples/upgrade-cascade src/usr/Cascade
```

The new `/usr/Cascade/` domain is picked up automatically by the System initd's `/usr/[A-Z]*/initd.c` iteration.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh upgrade-cascade
```

Expected sentinels in `src/usr/Cascade/data/test-result.log`:

```
Cascade:test: starting
Cascade:test: CLONE SETUP OK
Cascade:test: STAGED SOURCE INERT OK
Cascade:test: UPGRADE ACCEPTED OK
Cascade:test: CASCADE PROPAGATE OK
Cascade:test: LIB BEHAVIOR OK
Cascade:test: CLONE STATE SURVIVED OK
Cascade:test: CLONE PATCH OK
```
