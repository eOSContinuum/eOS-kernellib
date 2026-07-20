# Execution model

The platform runs one task at a time, to completion, with no preemption. This document states that model precisely enough to reason about concurrency, latency, and ordering; the mechanics behind each piece are documented where they live and linked below rather than repeated here.

**Audience**: an application author or evaluator reasoning about concurrency, latency, and ordering: deciding whether a piece of work fits inside one task, whether it needs `call_out` chunking, or what "the platform is slow" actually means here.

## What starts a task

A task begins when the host driver calls into LPC. The triggers:

- **Input on a connection.** A new connection (`telnet_connect` / `binary_connect` / `datagram_connect` in `src/kernel/sys/driver.c`) or a line of input on an existing one (`receive_message`, threaded through `src/kernel/lib/connection.c` to the user object) each start a task.
- **A `call_out` firing.** The scheduling call returns immediately; the deferred call runs later as its own task (`docs/lpc-essentials.md` Deferred work: call_out).
- **Boot initialization.** The cold-boot initd cascade (`src/usr/System/initd.c`'s `create()` and everything it compiles) runs as the driver's own call into LPC (`docs/architecture.md` Boot sequence).
- **A driver hook.** Host-level events the driver reports into LPC include a kill signal (`interrupt()`), a library recompile (`recompile()`), and a program removal (`remove_program()`). Each arrives as a call into `src/kernel/sys/driver.c`, "the object DGD calls into for fundamental events" (`docs/architecture.md` Boot sequence).

Whatever the trigger, exactly one task is running in the process at any instant.

## Run to completion

A task runs to completion before the next one begins. There is no preemption and no interleaving: LPC has no threads (`docs/lpc-essentials.md` Deferred work: call_out), and the driver never suspends one task's call stack to service another. The doc set's glossary names this same execution-time unit a *timeslice*: "the execution-time unit DGD's scheduler uses between atomic operations" (`docs/glossary.md` timeslice). It also states the boundary from the persistence side: statedumps occur between timeslices, never inside an atomic operation (`docs/persistence.md` The statedump cycle).

This single serialization point is why application code needs no locks: two callers contending for the same state never run at the same instant, so a write always sees the prior write's committed result, not a torn intermediate. It is the foundation of the multi-agent-coherence primitive. See `docs/runtime-primitives.md` §7, whose demonstration is exactly two tasks racing for one resource and losing the race deterministically rather than corrupting it.

## What serialization does not give you

The no-locks guarantee is scoped to one task. Within a task, no other code runs; between tasks, anything can. A logical operation that spans a task boundary re-admits the interleaving the single task removed: other tasks run between your first task and its continuation, and state read in the first may be stale by the second.

The platform ships several surfaces that split one logical operation across tasks, and each is a place this bites:

- **`call_out` chunking slices** (The price below; `docs/application-authoring.md` Spreading work across timeslices): every fired slice is a fresh task, and other tasks run between slices.
- **HTTP body receipt** (`docs/http-applications.md` Call `expectEntity` for body-bearing methods): the request line and headers parse in one task, the body arrives in another, and the application itself carries state between them (the `pendingRequest` idiom).
- **Outbound-connection callbacks** (`docs/application-authoring.md` Outbound connections): the connect, response, and close callbacks are each their own task.
- **Merry `$delay()` continuations** (`docs/merry-language.md`): the resume re-enters the script later, in a new task.

The discipline: make the decision and the write share one task -- re-read and re-validate at write time instead of acting on values a prior task read. `examples/chat-app` demonstrates the read-at-write-time half directly: `claim_slot` reads the member list at the instant it writes, so a second caller sees the first's committed result and yields (phase 9), while `claim_slot_stale` writes from a snapshot captured earlier and reproduces the lost update (phase 9b) -- `examples/chat-app/obj/room.c` carries both. That contrast runs within one task there; across a task boundary it is the same discipline against a stronger adversary, because now another task really does run between your read and your write. And if mid-operation state must never be visible to other tasks, the operation cannot span tasks at all -- shrink it into one, because there is no lock to take.

## The price: head-of-line latency

Serialization has a cost: a long-running task delays every other task waiting behind it, including other connections' input, other `call_out`s due to fire, and the next boot step. Nothing else in the process runs until the current task returns or errors.

Two things bound that cost instead of letting it become a hang:

- **The tick budget.** Every task runs under a per-owner resource ceiling. Exceeding it raises `Out of ticks` rather than blocking the platform indefinitely. This turns a runaway computation into an error, not a stall. The mechanics (what a tick charges, how the budget is set and read, atomic functions costing double) are in `docs/application-authoring.md` Writing tick-aware code. This document only needs the shape of the trade it makes.
- **`call_out` chunking.** For work that legitimately needs more than one task's budget, the standard idiom is to process a bounded slice, save a cursor, and re-arm with `call_out` for the next slice. Each slice is its own task with a fresh budget, so long work proceeds without holding up the queue for its whole duration. Worked example: `docs/application-authoring.md` Spreading work across timeslices.

## Under sustained load

What gives when offered load exceeds drain rate, stated from driver source, with the measured side in `docs/operations.md` Limits and capacity:

- **Input queues in the transport, not the image.** While a task runs, the driver reads no connection input: arriving bytes wait in the OS socket buffer, and TCP backpressure -- not platform memory growth -- is what slows a sender. The driver's own per-connection input buffer is small and fixed (2 KiB telnet, 8 KiB binary), refilled only between tasks.
- **Input can be deliberately paused per connection.** The connection layer's `MODE_BLOCK` applies the same backpressure on demand (`block_input`); the HTTP layer holds input blocked while a response drains and resumes on `MODE_UNBLOCK`.
- **Output buffers in the image, and the application layer's share is unbounded.** The driver caps its per-connection output string, but the HTTP connection library above it accumulates a `StringBuffer` with no cap. A consumer that drains slower than the application emits -- a slow server-sent-events subscriber is the canonical case -- grows that buffer in image memory for as long as the connection lives. No shipped policy caps or sheds it, and the console cannot end a specific connection: no verb exists, transport connection objects are kernel-owned and not destructible from the operator surface, and HTTP connections are not even enumerable there. The buffer lives exactly as long as the connection -- it empties at the client's pace and dies with the client's close, the application's own teardown, or a platform restart (`docs/operations.md` Network boundary and transport security names the perimeter controls and the locked-out recovery path).
- **The `users` table is the connection ceiling, and hitting it is silent.** At the cap the driver refuses to create the connection object; observed from a client, the TCP connect completes and nothing ever answers, and nothing is logged platform-side. Alert on the users count before it gets there (`docs/operations.md` Monitoring signals), and treat a climbing count under flat traffic as a connection leak (`docs/operations.md` Common failure modes).
- **The `call_outs` table errors at its cap.** Scheduling past it raises `Too many callouts`: the deferred-work backlog is bounded by config, and the producer sees the refusal.
- **One task still holds everything, but the hold is bounded.** No preemption exists (Run to completion above), so a saturated queue of near-budget tasks is the worst sustained case: the tick budget bounds any single task's hold, and the measured price of that worst case -- what a probing client observes while the queue is saturated -- is in `docs/operations.md` Limits and capacity beside the throughput figures.

## Atomicity is inside a task, not instead of it

An `atomic` function is a rollback envelope *inside* a task, not a second serialization mechanism. Declaring a function `atomic` does not change when it runs or whether it can be interrupted (every task already runs uninterrupted). It changes what happens if the function errors: its dataspace mutations, and its nested calls', roll back as a unit instead of leaving a partial write in place (`docs/lpc-essentials.md` Atomicity). A task with no `atomic` function anywhere on its call stack still runs to completion. It just has no rollback if it errors partway through a mutation.

## Deferred work ordering

A `call_out` scheduled during a task does not run inside that task, however short the delay: the scheduling call returns, the current task finishes, and the deferred call fires afterward as a new, independent task. It is an ordinary, non-atomic call unless the called function is itself declared `atomic` (`docs/lpc-essentials.md` Deferred work: call_out). `docs/dispatcher.md` names the new-task fact directly for an observer's `$delay` continuation: "the rest of the source resumes `seconds` later in a fresh `call_out` (a new task, a new dispatch batch)". The platform's logging facility depends on the same ordering for correctness: `logd` cannot write synchronously from inside an atomic dispatch, so it buffers the line and schedules `call_out(0)`, and the buffered batch is flushed to disk in that later, non-atomic task (`docs/operations.md` Logging and diagnostics). A `call_out(0)` fired from inside a task is not "immediate". It is "next", after the current task and anything already ahead of it in the queue.

## Where to next

- [`docs/runtime-primitives.md`](runtime-primitives.md) §7: the multi-agent-coherence primitive this serialization point provides, with its demonstration and open work.
- [`docs/application-authoring.md`](application-authoring.md) Writing tick-aware code: the tick budget mechanics and the `call_out` chunking idiom in full.
- [`docs/application-authoring.md`](application-authoring.md) Outbound connections: how an initiated connection's callbacks arrive as new tasks under this model, and the request-deadline idiom.
- [`docs/dispatcher.md`](dispatcher.md): batching, cascade bounds, and `$delay` continuations built on top of the task boundary.
