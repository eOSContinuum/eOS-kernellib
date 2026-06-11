# Minimal signal application

The smallest possible signal-on-property demonstration: one property
host, one Merry observer, one write, one assertion. The walkthrough --
including why this primitive is worth a runtime rather than external
glue -- lives in `docs/signal-applications.md`.

## Operations

- A clonable inherits `/lib/util/properties` -- the only requirement to host both observed state and observer bindings.
- The test driver registers a one-line Merry reaction (`Set($this, "signal:fired", 1)`) for the `signal:watched` property, writes the property once, and asserts the marker landed by the time `set_property` returned: the dispatcher fires observers synchronously inside the write.

## Deployment

```sh
cp -R examples/signal-app src/usr/SignalApp
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically.

## Verify

```sh
rm -f .runtime/state/snapshot .runtime/state/snapshot.old
scripts/setup-runtime.sh
cp -R examples/signal-app .runtime/src/usr/SignalApp
.runtime/bin/dgd mva.dgd &
sleep 5
kill %1
cat .runtime/src/usr/SignalApp/data/test-result.log
```

Expected result-log contents:

```
SignalApp:test: starting
SignalApp:test: SIGNAL OK
```

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot.
- `lib/app.c` -- thin marker lib mirroring the other bundled examples.
- `obj/thing.c` -- the property host; a single `/lib/util/properties` inherit.
- `sys/test.c` -- boot-time test driver; registers the observer, writes the property, asserts the reaction fired.

## Notes

- Registration rides the capability gate's domain-match path: the driver and the host live in the same domain, so no approved-registrar entry is needed.
- Everything else the platform offers (ur-inheritance for observer sharing, logical names, schema-backed persistence, batching, timings) composes onto this same property store; `docs/signal-applications.md` points at where each is demonstrated.
