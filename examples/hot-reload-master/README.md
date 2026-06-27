# Hot-reload-master demonstration

**Audience**: application authors and operators wanting empirical evidence that recompiling a master in place propagates the new program to its existing clones while each clone keeps its own state — the load-bearing half of the hot-reload primitive (`docs/runtime-primitives.md` section 4, `docs/code-lifecycle.md`).

A minimal headless example. `obj/counter` is a clonable master with a `count` variable and a `version()` string. The boot-time driver `sys/test` clones it, advances the count, recompiles the master with new source whose `version()` returns a different value, and — from the next dispatch — confirms the existing clone runs the new program (`PROPAGATE`) while still reporting its old count (`STATE SURVIVED`).

This complements `hot-reload-demo`, which shows single-object replacement over HTTP. Neither exercises the library-inheritance cascade — a recompiled parent library re-touching its inheritors through the upgrade daemon (`/usr/System/sys/upgraded.c`); that is a separate, heavier mechanism.

## Layout

- `obj/counter.c` — clonable master: `count` state, `bump()`, `query()`, `version()`. Clonable objects live under `obj/` (the kernel's `clone_object` only accepts paths containing `/obj/`).
- `sys/test.c` — boot-time driver; writes sentinels to `data/test-result.log`.
- `initd.c` — compiles the counter and the driver at boot.

## Deployment

Copy the directory into the kernel layer's `src/usr/` as the `Reload` domain:

```sh
cp -R examples/hot-reload-master src/usr/Reload
```

The new `/usr/Reload/` domain is picked up automatically by the System initd's `/usr/[A-Z]*/initd.c` iteration.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh hot-reload-master
```

Expected sentinels in `src/usr/Reload/data/test-result.log`:

```
Reload:test: starting
Reload:test: CLONE STATE OK
Reload:test: PROPAGATE OK
Reload:test: STATE SURVIVED OK
```
