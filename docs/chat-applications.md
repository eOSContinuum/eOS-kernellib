# Writing chat applications

A chat application on eOS-kernellib presents multi-user messaging as a thin layer over the platform's runtime primitives. Rooms are clonable property-bearing objects with member lists and message logs; users are clonable property-bearing objects with subscriptions and capability tokens; admin authority is a Light-Weight Object attached to the user that the admin daemon validates at verb entry. The example grows incrementally as additional runtime primitives are demonstrated against the same chat-room host: capability separation lands first, persistence across statedump second, sandboxed code load third, async-event semantics fourth, multi-agent coherence last.

**Audience**: an application author building a multi-user service on eOS-kernellib; comfortable with LPC syntax (or read `docs/lpc-essentials.md` first); has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers, the daemon contracts, and the auto-inheritance chain that any application picks up. `docs/runtime-primitives.md` enumerates the eight platform-owned guarantees this example exercises. `docs/dispatcher.md` covers the property-change dispatcher; several patterns below register observers against it.

## Reference application

`examples/chat-app/` carries a working reference implementation: a domain initd, Room and User clonables, an admin-token LWO, chat and admin daemons, and a boot-time test driver. The code there is the canonical example -- accurate, compiling, and runnable.

The sections below explain the build patterns the reference application demonstrates. Read the code in `examples/chat-app/` alongside this document; the phase-by-phase account of what the test driver proves lives with the driver, in `examples/chat-app/README.md` Phase notes.

## Quick start

Deploy, run, and verify the reference application before reading the walkthrough. Assumes the setup from `docs/getting-started.md` (DGD built; `example.dgd`'s `directory` pointing at this repository's `src/`; a `state/` directory beside it).

```sh
# Deploy: System initd picks up /usr/[A-Z]*/initd.c automatically.
cp -R examples/chat-app src/usr/Chat

# Boot 1 (cold): the test driver runs phases 1 through 9e and 10 at boot,
# dumps a snapshot at t+5, and exits on its own.
/path/to/dgd/bin/dgd example.dgd

# Boot 2 (restore from snapshot): the persistence-verify call_out captured
# in the snapshot fires and appends PERSIST-VERIFY OK.
/path/to/dgd/bin/dgd example.dgd state/snapshot &
sleep 5
kill %1

# Boot 3 (cold again, snapshot NOT loaded): the negative case appends
# COLDBOOT-LOST OK -- in-memory state does not survive without the snapshot.
/path/to/dgd/bin/dgd example.dgd &
sleep 3
kill %1
```

The test driver's twenty assertions write sentinel lines to `src/usr/Chat/data/test-result.log`; each passing phase appends a line ending in ` OK`:

```sh
grep -c ' OK' src/usr/Chat/data/test-result.log
```

The example's `README.md` lists the expected result-log contents per boot. For a fresh re-run, clear the deployed domain and the state files first (`rm -rf src/usr/Chat; rm -f state/snapshot* state/swap`) -- the marker and result files live in the deployed source tree, and a stale snapshot changes what boot 1 restores.

## Application layout

The minimum chat application is eight files:

```text
src/usr/Chat/
  initd.c             - domain initd; compiles room/user/admin_token/chat/admin/test
  lib/
    app.c             - marker library (shared inherit chain for sys/*)
  obj/
    room.c            - ChatApp:Room clonable
    user.c            - ChatApp:User clonable
  data/
    admin_token.c     - capability LWO
  sys/
    chat.c            - chat dispatcher daemon
    admin.c           - admin daemon (capability-gated verbs)
    test.c            - boot-time test driver
```

The `obj/` / `sys/` / `data/` / `lib/` discipline matches `docs/architecture.md`: clonables under `obj/`, daemons under `sys/`, Light-Weight Objects under `data/`, inheritable libraries under `lib/`.

## Boot-order constraint

`/usr/System/initd::create()` iterates `/usr/[A-Z]*/initd.c` alphabetically after a fixed `TLS`, `HTTP`, `LPC` prefix. The Chat domain fires earlier than Merry, Schema, and Vault, so a cross-domain call from `sys/chat::create()` into one of those daemons at compile-time would hit a not-yet-loaded master. The test driver defers all cross-domain work to a `call_out("setup_and_run", 0)` from its `create()`; that call_out fires after every per-domain initd has returned.

The chat and admin daemons themselves do not call into other domains during their `create()`. They are inert until the test driver invokes them (or, in a real deployment, until a transport-layer handler routes a request to them).

## Capability separation

The example demonstrates capability separation by gating each admin verb on a per-room capability token attached to the actor's user object. The two halves are observable in `sys/admin.c`:

- **Verification**: `_check_admin_token(actor, room, action)` walks `actor->query_admin_tokens()`, matches the first token whose `query_room()` is the target room (or `nil` for realm-wide), and confirms `action` appears in `query_actions()`. No matching token surfaces an `error()` that the verb's caller must `catch{}`.
- **Issuance**: `grant_admin(grantor, subject, room, actions)` mints an `admin_token` LWO via `new_object(TOKEN_LWO)`, sets its fields from the arguments, and attaches it to `subject->add_admin_token(token)`. A non-`nil` grantor must itself hold a `"grant"` action for the same room; a `nil` grantor is reserved for the test driver's bootstrap path and is gated by the domain-isolation discipline (only the Chat domain can call `Chat:admin::grant_admin` with a `nil` grantor).

The shape mirrors `/usr/Merry/sys/merry::_check_registrar` (the capability gate for observer registration): a private helper that takes inputs the public LFUN captured at entry and throws on rejection. The error message names the actor and the requested action so failures in a smoke transcript or admin-console log have enough context to diagnose.

The capability mechanism is the gate, not the verb's identity or the caller's program: the same `kick` LFUN rejects one actor and accepts another because the gate is parameterized on the actor's token holdings, not on the program performing the call (phases 1 and 2).

## Persistence

The persistence phases demonstrate richer persistent state: a chat session is established, the runtime is dumped to a snapshot, DGD restarts against the snapshot, and the whole session is shown to have survived -- messages, member lists, and a cross-clone object reference resurrected as the *same* object, not a copy. The chat application writes no serialization code, defines no schema, and opens no database connection -- the runtime image *is* the durable store. Two authoring facts carry the pattern: DGD does not re-run `create()` on restored objects (it resumes the dumped image), and `call_out`s survive the snapshot and fire once their scheduled times have elapsed -- which is how the example's restore-side verification triggers at all.

The lesson is the boundary of orthogonal persistence. *Snapshot + restore* preserves the entire in-image state -- properties **and** plain LPC globals alike; there is no "globals vs Vault" difference at that granularity (the `merry-app` walkthrough makes the same point). What in-image state does *not* survive is a *cold boot without a snapshot*: a from-scratch start finds an empty world. Durability beyond the image snapshot -- surviving a from-scratch boot, a lost snapshot, or migration to another host -- is the job of an on-disk store: the Schema-registered Vault Marshal path that `examples/vault-app/` demonstrates. The chat-app room is in-memory-only (it inherits `/lib/util/properties` but is not Schema-registered), so its state rides the snapshot, not the disk.

## Sandboxed reactions

A room can load and run *untrusted code*. A per-room reaction is a Merry script -- source compiled at runtime via `new_object("/usr/Merry/data/merry", source)` and bound on the room object as a property. The script executes inside the Merry sandbox: a kfun is callable from a Merry script only if no `SANDBOX()` shadow in `/usr/Merry/lib/merrynode.c` masks it. That deny list is the security boundary -- a room can accept a reaction written by an untrusted author and run it, confident that the author cannot reach `write_file`, `clone_object`, `open_port`, `shutdown`, or any of the other entries in the 51-entry `SANDBOX()` deny list (plus the explicit `call_other` and `new_object` shadows; see the deny-list reference in `docs/merry-language.md`).

The reaction-property key follows the `merry:on:<path>:<timing>` convention the property-change dispatcher reads (`docs/dispatcher.md`), and registration order is firing order.

The deny-by-shadow mechanism is *implicit whitelist by presence*: there is no allow list to maintain. A kfun is reachable from a Merry script only if no `SANDBOX()` shadow masks it; adding a new shadow line denies a kfun, removing one permits it. The compile/fire split matters for the security model -- registration of a deny-listed reaction **succeeds** (the compiler resolves the kfun name to the shadow, so the symbol exists), and the boundary is enforced at execution, at the first fire. An untrusted author cannot tell at registration time whether their reaction will be stopped.

## Async events

`post_message` does not append to the message log itself. It resolves the message's `@name` mentions against the room roster, then writes a single `chat-room.message` arrival signal via `set_property`. That one write is what the dispatcher fans out: a room's main-timing append-to-history observer records the message, and (where registered) a post-timing mention-notify observer delivers a cross-user notification. The sender's code contains no notification logic -- message arrival is a first-class dispatched event, not a daemon side effect.

Event notification is **synchronous, inside the same write that caused the state change**. The "asynchronous" part is the agent-side experience: the sender registered a listener and never polled, but the listener fires within the dispatch, not on a later tick. That synchronous-in-envelope mechanism is what makes the notification atomic with the state change -- if the change rolls back, the notification rolls back with it. A queued or `$delay`-deferred notification would escape the envelope and fire anyway, which is precisely why the platform's event notification is synchronous, not queued.

Deferring work to a later tick is a *different* capability, and the example keeps it separate so the two are not conflated: a `$delay()` continuation inside a dispatcher-fired observer returns to the dispatcher immediately and completes on a subsequent tick, outside the triggering write's atomic envelope (`docs/dispatcher.md` Deferred observer continuations; the host object exposes the standard `delayed_call` / `perform_delayed_call` glue, and the observer stays pure Merry).

## Multi-agent coherence

The load-bearing guarantee is write coherence: when two users contend for the same shared state, the runtime serializes the writes so exactly one succeeds and the other observes the committed result. The platform provides this without locks or a coordination protocol -- it falls out of reading current state at write time inside the runtime's single-threaded atomic-task scheduling.

`obj/room.c` carries a bounded resource: a capacity and a `claim_slot` that takes a slot only while the room is below capacity.

```c
int claim_slot(object user)
{
    object *current;
    int cap;

    cap = query_capacity();
    current = query_members();           /* current state, read at write time */
    if (cap > 0 && sizeof(current) >= cap) {
        return 0;                        /* room full -- the loser sees the update */
    }
    if (!member(user, current)) {
        set_property("chat-room.member-list", current + ({ user }));
    }
    return 1;
}
```

Each call is its own atomic DGD task, so the runtime runs contenders one after another: the first reads current state and commits; the second's read at write time already reflects that commit and is refused. There is no lock and no retry loop. The anti-pattern is writing from a snapshot captured before a concurrent commit -- that is the classic lost update, and it appears the moment a writer acts on stale state instead of re-reading at write time (phases 9 and 9b prove both sides). The read-side companion: readers of one runtime property see one state; divergence appears only when an application caches a private copy the runtime already keeps coherent (phase 9c).

Two compositions extend the guarantee:

- **Atomicity across objects**: run through `MERRY->batch(..., ([ "atomic": 1 ]))`, a multi-room write is one atomic unit -- commit lands all rooms, a throw rolls all of them back; the same failing writer run through a non-atomic batch leaves partial state on error (phase 9d). The all-or-nothing boundary the atomic batch provides is exactly what the non-atomic path leaves to the application to clean up.
- **Cohort behavior via UrHierarchy**: a base room carries the observer; child rooms that set the base as their ur-object inherit the reactive behavior with `$this` bound to each child, so one ancestor registration serves the cohort -- and a room with neither its own observer nor an ancestor's records nothing (phase 9e).

The full agent-to-agent conflict scenario -- agents with identity wrappers contending through an inter-agent protocol -- is a downstream concern for an agent-identity layer. The substrate serialization above is what such a wrapper would expose at the agent-facing surface.

## Verification -- the chat-app sentinels

The boot driver proves every claim above on a live boot cycle: `DGD_BIN=... scripts/run-example.sh chat-app` (three boots: cold, restore, cold-without-snapshot). The per-phase walkthrough -- driver mechanics, boot-log excerpts, the `[caught]` annotation convention -- lives beside the driver in `examples/chat-app/README.md` Phase notes.

| Claim | Phase | Sentinel |
|---|---|---|
| The capability gate refuses an untokened actor, state unchanged | 1 | `CAP-REJECT OK` |
| The same verb accepts a tokened actor | 2 | `CAP-ACCEPT OK` |
| A registered in-sandbox reaction auto-fires on message arrival | 3 | `SANDBOX-ACCEPT OK` |
| A deny-listed kfun is stopped at first fire, with no side effect | 4 | `SANDBOX-REJECT OK` |
| A cross-user notification lands inside the triggering write | 5 | `EVENT-ATOMIC OK` |
| No observer registered, no reaction fired | 6 | `NO-REACT OK` |
| A rolled-back write rolls its notification back with it | 7 | `EVENT-ROLLBACK OK` |
| A `$delay` continuation completes later, outside the envelope | 8 | `DEFERRED-OP OK` |
| Contended writes serialize; the loser sees the committed state | 9 | `COHERENCE-SERIALIZE OK` |
| A stale-snapshot write loses an update | 9b | `LOST-UPDATE OK` |
| Readers of one property see one state | 9c | `COHERENCE OK` |
| A cached private copy diverges from the live property | 9c | `CACHED-DIVERGE OK` |
| An atomic batch commits across two rooms | 9d | `ATOMIC-COMMIT OK` |
| An atomic batch rolls back across two rooms | 9d | `ATOMIC-ROLLBACK OK` |
| A non-atomic failure leaves partial state | 9d | `PARTIAL-STATE OK` |
| One ancestor registration serves the cohort | 9e | `ANCESTRY OK` |
| No registration anywhere, no reaction | 9e | `NO-INHERIT OK` |
| A live session is dumped to a snapshot | 10 | `PERSIST-SETUP OK` |
| The restored image resumes the whole session, references rebound | 11 | `PERSIST-VERIFY OK` |
| A cold boot without the snapshot loses in-memory state | negative | `COLDBOOT-LOST OK` |

## What this example does not exercise

The chat-app reference is the multi-user messaging surface, not a MUD. The following adjacent concerns are deliberately out of scope:

- **Transport layer**: no telnet listener, no HTTP route handler, no WebSocket upgrade path. The test driver `IS` the client -- it simulates user sessions via direct LFUN calls. `examples/composite-app/` demonstrates the transport composition ([composite-applications.md](composite-applications.md)); wiring the chat domain specifically remains future work.
- **Multi-realm presence**: rooms and users live in a single realm. Presence is per-room, not per-realm-per-room.
- **Game content**: no SAM markup, no body/avatar layer, no light/bulk/proximity machinery, no stance/move/social verbs, no theme system, no voice/video chrome.
- **Message retention policies**: messages append to `chat-room.message-log` without bound. A real deployment would enforce per-room retention via a policy daemon.
- **Per-message edit/delete**: messages are append-only in the example. Edit/delete would arrive as additional admin verbs gated by per-room capability.

## Where to next

- [`examples/chat-app/README.md`](../examples/chat-app/README.md) -- run recipes and the phase-by-phase driver walkthrough (Phase notes).
- [`examples/chat-app/sys/test.c`](../examples/chat-app/sys/test.c) -- the canonical boot-time test driver.
- [`docs/dispatcher.md`](dispatcher.md) -- the property-change dispatcher (observers, timings, deferred continuations).
- [`docs/persistence.md`](persistence.md) -- the durable-state primitives.
- [`docs/architecture.md`](architecture.md) -- the capability tiers and daemon contracts that frame this application.
- [`docs/runtime-primitives.md`](runtime-primitives.md) -- the runtime primitives the chat application exercises.
