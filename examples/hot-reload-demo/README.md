# Hot-reload demonstration

**Audience**: application authors and operators wanting empirical evidence that LPC source can be recompiled into the live runtime via `compile_object`, with the next dispatch picking up the new program — no DGD restart, no separate deploy step.

A minimal HTTP/1 application that demonstrates the platform's hot-reload primitive. A target object (`greeting`) exposes a single method; an HTTP route accepts new LPC source as a request body, recompiles the target, and the next read returns the new behavior.

The HTTP boundary is convenient for an end-to-end smoke, not load-bearing — `compile_object` is the platform mechanism and works from any dispatch path. See `docs/runtime-primitives.md` §4 for the platform guarantee and its open empirical questions.

## Routes

- `GET /greet` — call `greeting->greet()` and return the result as plain text.
- `POST /compile` — body is treated as LPC source. Calls `compile_object("/usr/WWW/greeting", body)`, replacing the master's program in place. Returns `Compiled <object_name>` on success.
- any other path — `404 Not Found`.

## Deployment

The platform's HTTP/1 bootstrap (`src/usr/System/sys/http_server.c`) clones the application at the kernel-defined path `/usr/WWW/obj/server`. To deploy this example, copy the directory into the kernel layer's `src/usr/`:

```sh
cp -R examples/hot-reload-demo src/usr/WWW
```

Then start the driver against a `.dgd` configuration that points at the kernel-layer source. The new `/usr/WWW/` user-layer domain is picked up automatically by the System initd's `/usr/[A-Z]*/initd.c` iteration.

## Verify

The smoke script exercises the three-step probe.

```sh
./smoke.sh
```

Expected output:

```
=== hot-reload-demo smoke against http://127.0.0.1:8080 ===

Step 1: GET /greet (cold-boot response)
  hello before recompile

Step 2: POST /compile (new LPC source for /usr/WWW/greeting)
  Compiled /usr/WWW/greeting

Step 3: GET /greet (post-recompile response)
  hello after recompile

=== PASS: post-recompile response contains expected marker ===
         initial: hello before recompile
         final:   hello after recompile
         (hot reload verified, no DGD restart; response changed across recompile)
```

The response changing across the three-step probe is the hot-reload evidence: the same DGD process serves the cold-boot string in step 1 and the recompiled string in step 3, with `compile_object` (invoked through `POST /compile`) as the only mechanism that changed in between.

The example also verifies headless, with no running server or HTTP client: the sentinel profile runs the same three-step reload from a boot-time driver (`sys/test.c`), asserting the `greet()` return value before the in-process `compile_object` and again from the next dispatch after it.

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh hot-reload-demo
```

`PASS` after 2 ` OK` sentinels (`INITIAL OK`, `RELOAD OK`) is the pass signal.

## Limits the demonstration does not cover

- **In-flight calls during recompile.** The host-runtime guarantee is that calls already dispatched against the old master finish on the old program; only subsequent dispatches see the new program. A sequential `curl` smoke does not exercise this — the cold-boot GET finishes before the POST arrives, and the second GET starts after the recompile commits. Verifying the in-flight-finishes-with-old half requires a concurrent-request probe (out of scope for this example).
- **Library cascade.** Recompiling `greeting.c` updates the `greeting` master only. If `greeting` inherited a parent library, the parent's children are not recompiled by this mechanism. See `docs/code-lifecycle.md` "Library upgrade: recompile cascade through dependents" for the platform's surface.
- **Behavior under host-driver extensions.** A driver extension that maintains a compiled-code cache must invalidate or re-key per `compile_object`; if it does not, the next dispatch may run stale native code. Empirically unverified for any specific extension. See `docs/runtime-primitives.md` §4 Open.

## Files

- `greeting.c` — the target master. Single `greet()` function returning a string.
- `obj/server.c` — per-connection HTTP/1 server (clonable). Inline routing for GET /greet and POST /compile.
- `initd.c` — domain initd; compiles `greeting` and `obj/server` at boot.
- `sys/test.c` — boot-time test driver: the same reload sequence in-process, with the post-recompile read deliberately made from a fresh dispatch, backing the headless `run-example.sh` profile.
- `smoke.sh` — POSIX-sh three-call end-to-end verification script.
