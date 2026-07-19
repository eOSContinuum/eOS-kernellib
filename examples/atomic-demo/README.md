# Atomic-rollback demonstration

**Audience**: application authors who want empirical evidence of the platform's atomicity primitive, and platform readers verifying that the runtime honors atomic-function rollback under a deliberate-failure path.

A minimal HTTP/1 application that demonstrates the `atomic` modifier's transactional-rollback semantics. A counter mutates its private state inside an `atomic` function body that errors; the host runtime rolls the mutation back; the next read of the counter shows the pre-call value.

The counter is the demonstration; the HTTP server is the routing surface. The same pattern works from any dispatch path — the HTTP boundary is convenient for an end-to-end smoke, not load-bearing.

## Routes

- `GET /counter` — return the current counter value as `counter=N\n`.
- `POST /increment-with-failure` — call `increment_with_failure()`. The route catches the deliberate error and reports it in the body. The atomic rollback fires regardless of catch.
- any other path — return `404 Not Found`.

## Deployment

The platform's HTTP/1 bootstrap (`src/usr/System/sys/http_server.c`) clones the application at the kernel-defined path `/usr/WWW/obj/server`. To deploy this example, copy the directory into the kernel layer's `src/usr/`:

```sh
cp -R examples/atomic-demo src/usr/WWW
```

Then start the driver against a `.dgd` configuration that points at the kernel-layer source. The new `/usr/WWW/` user-layer domain is picked up automatically by the System initd's `/usr/[A-Z]*/initd.c` iteration.

## Verify

The smoke script exercises the three-step probe and asserts the rollback.

```sh
./smoke.sh
```

Expected output (initial counter value will vary across runs):

```
=== atomic-demo smoke against http://127.0.0.1:8080 ===

Step 1: GET /counter
  counter=0

Step 2: POST /increment-with-failure
  deliberate-failure-fired: deliberate failure for atomic-rollback demonstration

Step 3: GET /counter (after deliberate failure)
  counter=0

=== PASS: counter unchanged across deliberate-failure increment ===
         initial=0   final=0   (rollback verified)
```

The unchanged counter across step 1 and step 3 is the rollback evidence. The `[atomic]` annotation in the boot log on the deliberate-failure trace is the runtime's own marker that the atomic envelope fired.

If the smoke reports `FAIL: counter changed`, the `atomic` modifier in `counter.c` is the binding piece — drop it and the same probe shows the mutation persisting.

The example also verifies headless, with no running server or HTTP client: the sentinel profile performs the same caught-failure increment from a boot-time driver (`sys/test.c`), asserting the pre-call baseline, the deliberate error's own text (proof the atomic body ran to its `error()`), and the unchanged counter after the catch.

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh atomic-demo
```

`PASS` after 3 ` OK` sentinels (`INITIAL OK`, `BODY-RAN OK`, `ROLLBACK OK`) is the pass signal.

## Files

- `counter.c` — the counter master. Holds the private int and the `atomic` increment.
- `obj/server.c` — per-connection HTTP/1 server (clonable). The platform clones one per incoming connection. Inline routing to the counter.
- `initd.c` — domain initd; compiles `counter`, `obj/server`, and `sys/test` at boot.
- `sys/test.c` — boot-time test driver: the same caught-failure increment in-process, backing the headless `run-example.sh` profile.
- `smoke.sh` — POSIX-sh end-to-end verification script.
