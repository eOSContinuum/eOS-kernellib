# Execution model

The platform runs one task at a time, to completion, with no preemption. This document states that model precisely enough to reason about concurrency, latency, and ordering; the mechanics behind each piece are documented where they live and linked below rather than repeated here.

**Audience**: an application author or evaluator reasoning about concurrency, latency, and ordering — deciding whether a piece of work fits inside one task, whether it needs `call_out` chunking, or what "the platform is slow" actually means here.

## What starts a task

A task begins when the host driver calls into LPC. The triggers:

- **Input on a connection.** A new connection (`telnet_connect` / `binary_connect` / `datagram_connect` in `src/kernel/sys/driver.c`) or a line of input on an existing one (`receive_message`, threaded through `src/kernel/lib/connection.c` to the user object) each start a task.
- **A `call_out` firing.** The scheduling call returns immediately; the deferred call runs later as its own task (`docs/lpc-essentials.md` Deferred work: call_out).
- **Boot initialization.** The cold-boot initd cascade — `src/usr/System/initd.c`'s `create()` and everything it compiles — runs as the driver's own call into LPC (`docs/architecture.md` Boot sequence).
- **A driver hook.** Host-level events the driver reports into LPC — a kill signal (`interrupt()`), a library recompile (`recompile()`), a program removal (`remove_program()`) — each arrive as a call into `src/kernel/sys/driver.c`, "the object DGD calls into for fundamental events" (`docs/architecture.md` Boot sequence).

Whatever the trigger, exactly one task is running in the process at any instant.

## Run to completion

A task runs to completion before the next one begins. There is no preemption and no interleaving: LPC has no threads (`docs/lpc-essentials.md` Deferred work: call_out), and the driver never suspends one task's call stack to service another. The doc set's glossary names this same execution-time unit a *timeslice* — "the execution-time unit DGD's scheduler uses between atomic operations" (`docs/glossary.md` timeslice) — and states the boundary from the persistence side: statedumps occur between timeslices, never inside an atomic operation (`docs/persistence.md` The statedump cycle).

This single serialization point is why application code needs no locks: two callers contending for the same state never run at the same instant, so a write always sees the prior write's committed result, not a torn intermediate. It is the foundation of the multi-agent-coherence primitive — see `docs/runtime-primitives.md` §7, whose demonstration is exactly two tasks racing for one resource and losing the race deterministically rather than corrupting it.

## The price: head-of-line latency

Serialization has a cost: a long-running task delays every other task waiting behind it — other connections' input, other `call_out`s due to fire, the next boot step. Nothing else in the process runs until the current task returns or errors.

Two things bound that cost instead of letting it become a hang:

- **The tick budget.** Every task runs under a per-owner resource ceiling; exceeding it raises `Out of ticks` rather than blocking the platform indefinitely. This turns a runaway computation into an error, not a stall. The mechanics — what a tick charges, how the budget is set and read, atomic functions costing double — are in `docs/application-authoring.md` Writing tick-aware code; this document only needs the shape of the trade it makes.
- **`call_out` chunking.** For work that legitimately needs more than one task's budget, the standard idiom is to process a bounded slice, save a cursor, and re-arm with `call_out` for the next slice — each slice its own task with a fresh budget, so long work proceeds without holding up the queue for its whole duration. Worked example: `docs/application-authoring.md` Spreading work across timeslices.

## Atomicity is inside a task, not instead of it

An `atomic` function is a rollback envelope *inside* a task, not a second serialization mechanism. Declaring a function `atomic` does not change when it runs or whether it can be interrupted — every task already runs uninterrupted — it changes what happens if the function errors: its dataspace mutations, and its nested calls', roll back as a unit instead of leaving a partial write in place (`docs/lpc-essentials.md` Atomicity). A task with no `atomic` function anywhere on its call stack still runs to completion; it just has no rollback if it errors partway through a mutation.

## Deferred work ordering

A `call_out` scheduled during a task does not run inside that task, however short the delay: the scheduling call returns, the current task finishes, and the deferred call fires afterward as a new, independent task — an ordinary, non-atomic call unless the called function is itself declared `atomic` (`docs/lpc-essentials.md` Deferred work: call_out). `docs/dispatcher.md` names the new-task fact directly for an observer's `$delay` continuation — "the rest of the source resumes `seconds` later in a fresh `call_out` (a new task, a new dispatch batch)". The platform's logging facility depends on the same ordering for correctness: `logd` cannot write synchronously from inside an atomic dispatch, so it buffers the line and schedules `call_out(0)`, and the buffered batch is flushed to disk in that later, non-atomic task (`docs/operations.md` Logging and diagnostics). A `call_out(0)` fired from inside a task is not "immediate" — it is "next", after the current task and anything already ahead of it in the queue.

## Where to next

- [`docs/runtime-primitives.md`](runtime-primitives.md) §7 — the multi-agent-coherence primitive this serialization point provides, with its demonstration and open work.
- [`docs/application-authoring.md`](application-authoring.md) Writing tick-aware code — the tick budget mechanics and the `call_out` chunking idiom in full.
- [`docs/dispatcher.md`](dispatcher.md) — batching, cascade bounds, and `$delay` continuations built on top of the task boundary.
