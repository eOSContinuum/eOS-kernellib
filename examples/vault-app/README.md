# Reference Vault application

A minimal Vault application that runs on top of eOS-kernellib. Demonstrates the platform contracts a property-bearing persistent object honors: schema-driven typed-property marshaling, on-disk XML round-trip, and Index-mediated name resolution. The full walkthrough lives in `docs/vault-applications.md`.

## Operations

- A `MyApp:Thing` clonable carries one `lpc_str` property (`label`) and one `lpc_int` property (`count`).
- At boot, the test driver clones a thing, sets properties, names it `MyApp:demo:thing1`, calls `Vault->store`, destructs the clone, then `Vault->spawn_one_by_name` reloads it from the XML on disk and asserts the property values round-trip.
- The reloaded object is also looked up via `Index->query_object(name)` to verify Vault's restoration registers the object's logical name.

## Deployment

Copy the directory into the kernel layer's `src/usr/` (the MyApp destination is the example's choice; pick any `/usr/<Name>/` that doesn't conflict):

```sh
cp -R examples/vault-app src/usr/MyApp
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. The cross-domain inherit `~Vault/lib/vault_node` and the cross-domain clone of `~Schema/obj/schema_node` require both Vault and Schema to be globally readable -- the platform grants these in `src/usr/System/initd.c`.

## Verify

```sh
rm -f .runtime/state/snapshot .runtime/state/snapshot.old
scripts/setup-runtime.sh
.runtime/bin/dgd mva.dgd &
sleep 5
grep "MyApp:test" .runtime/state/boot.log
# Expect: MyApp:test: ROUND-TRIP OK
```

The on-disk artifact lands at `.runtime/state/vault/data/MyApp/demo/thing1.xml`. The Vault root resolves to `<data-dir>/data/MyApp/demo/thing1.xml` where `<data-dir>` is the runtime's data root.

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot.
- `lib/app.c` -- thin wrapper inheriting `~Vault/lib/vault_node`; daemons inherit this to participate in the Vault.
- `obj/thing.c` -- property-bearing clonable; carries `label` (string) + `count` (int).
- `sys/test.c` -- boot-time test driver; registers the `MyApp:Thing` schema and runs the round-trip assertion via `call_out("run_tests", 0)`.

## Notes

- The example uses a per-application schema (`MyApp:Thing`) registered at boot from `sys/test.c::create()` rather than relying on the core `Hierarchy` or `Entry` primitives. The core primitives describe structural relationships (ancestry, flat key-value), not typed application properties; per-application schemas are the natural surface for a typed property tree.
- The test driver writes assertion results via `DRIVER->message()` so they surface in `boot.log` while the platform's `sysLog` is still a no-op stub. When a real log facility lands, the driver can drop the `DRIVER->message` path and rely on `sysLog` alone.
