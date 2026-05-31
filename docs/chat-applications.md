# Writing chat applications

A chat application on eOS-kernellib presents multi-user messaging as a thin layer over the platform's runtime primitives. Rooms are clonable property-bearing objects with member lists and message logs; users are clonable property-bearing objects with subscriptions and capability tokens; admin authority is a Light-Weight Object attached to the user that the admin daemon validates at verb entry. The example grows incrementally as additional runtime primitives are demonstrated against the same chat-room host: capability separation lands first, persistence across statedump second, sandboxed code load third, async-event semantics fourth, multi-agent coherence last.

**Audience**: an application author building a multi-user service on eOS-kernellib; comfortable with LPC syntax (or read `docs/lpc-essentials.md` first); has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers, the daemon contracts, and the auto-inheritance chain that any application picks up. `docs/runtime-primitives.md` enumerates the eight platform-owned guarantees this example exercises. `docs/dispatcher.md` covers the property-change dispatcher; later phases of this walkthrough register observers against it.

## Reference application

`examples/chat-app/` carries a working reference implementation: a domain initd, Room and User clonables, an admin-token LWO, chat and admin daemons, and a boot-time test driver. The code there is the canonical example -- accurate, compiling, and runnable. To deploy it:

```sh
cp -R examples/chat-app src/usr/Chat
```

Then sync the runtime tree (`scripts/setup-runtime.sh`) and boot DGD against `mva.dgd`. The verify command in the example's README cats `src/usr/Chat/data/test-result.log` and counts " OK" lines after the smoke harness completes.

The sections below explain what the reference application is doing and why. Read the code in `examples/chat-app/sys/test.c` alongside this document.

## Application layout

The minimum chat application is eight files plus an initd:

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

`/usr/System/initd::create()` iterates `/usr/[A-Z]*/initd.c` alphabetically. The Chat domain fires earlier than Merry, Schema, and Vault, so a cross-domain call from `sys/chat::create()` into one of those daemons at compile-time would hit a not-yet-loaded master. The test driver defers all cross-domain work to a `call_out("setup_and_run", 0)` from its `create()`; that call_out fires after every per-domain initd has returned.

The chat and admin daemons themselves do not call into other domains during their `create()`. They are inert until the test driver invokes them (or, in a real deployment, until a transport-layer handler routes a request to them).

## Capability separation

The first revision of the example demonstrates capability separation by gating each admin verb on a per-room capability token attached to the actor's user object. The two halves are observable in `sys/admin.c`:

- **Verification**: `_check_admin_token(actor, room, action)` walks `actor->query_admin_tokens()`, matches the first token whose `query_room()` is the target room (or `nil` for realm-wide), and confirms `action` appears in `query_actions()`. No matching token surfaces an `error()` that the verb's caller must `catch{}`.
- **Issuance**: `grant_admin(grantor, subject, room, actions)` mints an `admin_token` LWO via `new_object(TOKEN_LWO)`, sets its fields from the arguments, and attaches it to `subject->add_admin_token(token)`. A non-`nil` grantor must itself hold a `"grant"` action for the same room; a `nil` grantor is reserved for the test driver's bootstrap path and is gated by the domain-isolation discipline (only the Chat domain can call `Chat:admin::grant_admin` with a `nil` grantor).

The shape mirrors `/usr/Merry/sys/merry::_check_registrar` (the capability gate for observer registration): a private helper that takes inputs the public LFUN captured at entry and throws on rejection. The error message names the actor and the requested action so failures in a smoke transcript or admin-console log have enough context to diagnose.

### Phase 1 -- rejected kick

The driver clones three users (alice, bob, carol) and one room. None of them carry an admin token at this point. bob calls `admin->kick(alice, room_a)`; the capability check finds no matching token and throws. The test driver's `catch{}` converts the throw into a `CAP-REJECT OK` sentinel and asserts that the membership of `room_a` did not mutate.

```text
ChatApp:test: starting
ChatApp:test: CAP-REJECT OK
```

The boot.log carries the `[caught]` annotation that DGD writes for any error-that-was-caught:

```text
** admin: actor bob not authorized for kick in room ChatApp:Room:LobbyA [caught]
                       /usr/Chat/sys/test
   58   setup_and_run         /usr/Chat/sys/test
   96 * run_tests             /usr/Chat/sys/test
                       /usr/Chat/sys/admin
   52   kick                  /usr/Chat/sys/admin
  148   _check_admin_token    /usr/Chat/sys/admin
```

The `[caught]` suffix confirms the error did not propagate to the runtime -- the test driver's `catch{}` swallowed it. DGD's convention is to log every error regardless of `catch{}` so the platform retains a trace; the caught/uncaught distinction is in the suffix.

### Phase 2 -- accepted kick

carol receives a per-room `"kick"` token via `admin->grant_admin(nil, carol, room_a, ({ "kick" }))`. The same `admin->kick(carol, alice, room_a)` call now passes the capability check; the verb removes alice from `room_a`'s member list and removes `room_a` from alice's subscription list. The test driver asserts both removals.

```text
ChatApp:test: CAP-ACCEPT OK
```

The two phases together demonstrate that the capability mechanism is the gate, not the verb's identity or the caller's program. The same `kick` LFUN rejects bob and accepts carol because the gate is parameterized on the actor's token holdings, not on the program performing the call.

## Persistence

The persistence phases demonstrate richer persistent state: a chat session is established, the runtime is dumped to a snapshot, DGD restarts against the snapshot, and the whole session is shown to have survived. The chat application writes no serialization code, defines no schema, and opens no database connection -- the runtime image *is* the durable store. This is persistent state as a runtime primitive, made concrete at the application layer. The phases follow the two-boot pattern of `examples/merry-app/sys/test.c` phases 16/17, plus a third (cold, no-snapshot) boot for the negative case.

### Phase 10 -- persist setup

The driver clones a fresh room (`ChatApp:Room:PersistD`), has alice and bob join it, and posts three messages through `chat::post_message`. It then sets alice's `chat-user.mention-tracker` to `({ bob })` -- a *cross-clone* object reference, alice's user object pointing directly at bob's. The room and the two accounts are saved as object globals on the test driver (non-`static` so the state dump captures them), the session shape is recorded to an on-disk marker file, a `persist_verify` `call_out` is scheduled at `t=3`, and the driver calls `/usr/System/sys/persist_helper::trigger_dump_and_exit`. That helper schedules `dump_state(FALSE)` + `shutdown()` on its own `call_out`, so the caller's stack unwinds before the snapshot is taken.

```text
ChatApp:test: PERSIST-SETUP OK
```

The snapshot captures everything reachable in the image: the room clone, the two user clones, the three message mappings (each carrying a `sender` object reference), alice's cross-clone mention-tracker reference, and the not-yet-fired `persist_verify` `call_out`. The driver then exits; the boot log records `** System halted.`

### Phase 11 -- persist verify

The smoke harness restarts DGD against the snapshot (`dgd mva.dgd .runtime/state/snapshot`). DGD does not re-run `create()` on restored objects; it resumes the dumped image and fires `call_out`s whose scheduled times have elapsed. The surviving `persist_verify` `call_out` fires immediately and asserts:

- the room still holds three messages and two members;
- `persist_alice` and `persist_bob` resolve to live clones with their `chat-user.name` values intact (the user accounts survived);
- alice's mention-tracker resurrected as the *same* bob object (`mention[0] == persist_bob`), not a copy -- DGD's object-reference resurrection rebound the cross-clone reference;
- a third user (carol) joining the restored room is admitted and observes the full prior session: three messages and a now-three-member roster.

```text
ChatApp:test: PERSIST-VERIFY OK
```

### Negative case -- cold boot without a snapshot

A third boot starts DGD cold *without* the snapshot argument. The driver's `setup_and_run` finds the on-disk marker present (a file -- it survives any boot) but `persist_room` `nil` (a cold boot resets all object globals). That combination is the signature of "a session was persisted but the snapshot was not loaded," so the driver writes the negative sentinel rather than re-running setup:

```text
ChatApp:test: COLDBOOT-LOST OK
```

The lesson is the boundary of orthogonal persistence. *Snapshot + restore* preserves the entire in-image state -- properties **and** plain LPC globals alike; there is no "globals vs Vault" difference at that granularity (the `merry-app` walkthrough makes the same point, listing "LPC global variables" among the guarantees its phase 16/17 cycle exercises). What in-image state does *not* survive is a *cold boot without a snapshot*: a from-scratch start finds an empty world. Durability beyond the image snapshot -- surviving a from-scratch boot, a lost snapshot, or migration to another host -- is the job of an on-disk store: the Schema-registered Vault Marshal path that `examples/vault-app/` demonstrates. The chat-app room is in-memory-only (it inherits `/lib/util/properties` but is not Schema-registered), so its state rides the snapshot, not the disk.

## Sandboxed reactions

A room can load and run *untrusted code*. A per-room reaction is a Merry script -- source compiled at runtime via `new_object("/usr/Merry/data/merry", source)` and bound on the room object as a property. The script executes inside the Merry sandbox: a kfun is callable from a Merry script only if no `SANDBOX()` shadow in `/usr/Merry/lib/merrynode.c` masks it. That deny list is the security boundary -- a room can accept a reaction written by an untrusted author and run it, confident that the author cannot reach `write_file`, `clone_object`, `open_port`, `shutdown`, or any of the other entries in the 51-entry `SANDBOX()` deny list (plus the explicit `call_other` and `new_object` shadows; see the deny-list reference in `docs/merry-language.md`).

These two phases load both sides of that boundary: an in-sandbox reaction that runs, and a sandbox-denied reaction that compiles but is stopped at its first fire. Phase 3's accepted reaction fires *automatically* off the dispatcher when a message is posted -- sandboxed code load and dispatcher auto-fire demonstrated together. Phase 4's rejected reaction is fired directly via `run_merry` on a distinct property path, so its sandbox-deny boundary is isolated from the message-arrival cascade.

The reaction-property key follows the `merry:on:<path>:<timing>` convention the property-change dispatcher reads (`docs/dispatcher.md`).

### Phase 3 -- accepted reaction

The driver registers two in-sandbox observers on the room's `chat-room.message` arrival signal at main timing, each calling only unshadowed merryfuns (`Set` / `Get` / `sizeof`, all routing through `merrynode.c`) -- an append-to-history observer and a message-count marker observer:

```c
MERRY->register_observer(room_a, "chat-room.message", "main",
    "Set($this, \"chat-room.message-log\", " +
    "Get($this, \"chat-room.message-log\") + ({ $new })); return TRUE;");
MERRY->register_observer(room_a, "chat-room.message", "main",
    "Set($this, \"chat-room.message-count\", " +
    "sizeof(Get($this, \"chat-room.message-log\"))); return TRUE;");
```

Registration order is firing order, so the marker observes the already-appended log. bob posts a message (he is still a member after the phase-2 kick removed alice); `post_message` writes `chat-room.message`, and the dispatcher fires both observers with **no manual `run_merry`**. The driver reads the appended log and the marker back and writes the sentinel:

```text
ChatApp:test: SANDBOX-ACCEPT OK
```

This is sandboxed code load *as a registered reaction*: scripts authored as strings, compiled at runtime, bound on a room, and executed against that room's property storage automatically on a real state change -- all within the sandbox surface.

### Phase 4 -- rejected reaction

The driver compiles a sibling reaction whose source calls `write_file`, a kfun in the `SANDBOX()` deny list:

```c
write_file("/usr/Chat/data/hack.txt", "pwned");
```

Registration **succeeds**: the Merry compiler resolves `write_file` to the local `SANDBOX(write_file)` shadow method, which exists, so compilation finds the symbol and binds the script. The error fires only on the *first invocation*, when the shadow body runs and raises:

```text
** function 'write_file' not allowed in merry code [caught]
```

The boot log carries the full call stack down to `merrynode.c` line 448 (the `SANDBOX(write_file)` shadow). The driver's `catch{}` converts the throw into a `SANDBOX-REJECT OK` sentinel and asserts that no `hack.txt` was created:

```text
ChatApp:test: SANDBOX-REJECT OK
```

The deny-by-shadow mechanism is *implicit whitelist by presence*: there is no allow list to maintain. A kfun is reachable from a Merry script only if no `SANDBOX()` shadow masks it; adding a new shadow line denies a kfun, removing one permits it. The compile/fire split matters for the security model -- an untrusted author cannot tell at registration time whether their reaction will be stopped, because the boundary is enforced at execution, not at load.

## Async events

`post_message` does not append to the message log itself. It resolves the message's `@name` mentions against the room roster, then writes a single `chat-room.message` arrival signal via `set_property`. That one write is what the dispatcher fans out: a room's main-timing append-to-history observer records the message, and (where registered) a post-timing mention-notify observer delivers a cross-user notification. The sender's code contains no notification logic -- message arrival is a first-class dispatched event, not a daemon side effect.

Event notification is **synchronous, inside the same write that caused the state change**. The "asynchronous" part is the agent-side experience: the sender registered a listener and never polled, but the listener fires within the dispatch, not on a later tick. That synchronous-in-envelope mechanism is what makes the notification atomic with the state change -- if the change rolls back, the notification rolls back with it.

### Phase 5 -- synchronous cross-user notification

A room carries an append-to-history main observer and a mention-notify post observer. alice posts a message mentioning dave; `scan_mentions` resolves `@dave` and carries him in the message's `mentions` list. The dispatcher fires the main append, then the post observer -- both synchronously:

```c
Set($new["mentions"][0], "chat-user.mention-tracker",
    Get($new["mentions"][0], "chat-user.mention-tracker") + ({ $this }));
return TRUE;
```

The instant `post_message` returns, dave's mention-tracker already names the room -- the notification landed inside the same `set_property` that wrote `chat-room.message`. `post_message` itself contains no notification logic:

```text
ChatApp:test: EVENT-ATOMIC OK
```

### Phase 6 -- no observer, no reaction

The same mention post, on a room with the append observer but **no** mention-notify observer registered. The dispatcher has nothing to fire at post timing, so the target's mention-tracker stays empty:

```text
ChatApp:test: NO-REACT OK
```

Reactions happen only where observers are registered -- a property write on an un-observed timing slot is inert. The negative isolates this to the missing post observer: the append observer *is* registered, so the message is still recorded.

### Phase 7 -- event atomic with state change

Because the notification is synchronous-in-envelope, it shares the atomic fate of the write. `atomic_post_then_fail` posts a message mentioning eve inside a DGD `atomic` function, then throws. The rollback unwinds every write the task made -- the message-log append *and* the observer's cross-user mention-tracker Set:

```text
ChatApp:test: EVENT-ROLLBACK OK
```

After the caught throw, the room has no message and eve has no notification: the event "did not fire" in the same sense the state change "did not occur". A queued or `$delay`-deferred notification would have escaped the envelope and fired anyway -- which is precisely why the platform's event notification is synchronous, not queued.

### Phase 8 -- a deferred operation, kept distinct

Deferring work to a later tick is a *different* capability, and the example keeps it separate so the two are not conflated. A post observer schedules a `$delay()` continuation that writes a delivery-receipt marker on a subsequent tick:

```c
$delay(2, FALSE);
Set($this, "chat-room.delivery-receipt", 1);
return TRUE;
```

`$delay(2, FALSE)` returns `FALSE` to the dispatcher immediately, so the receipt is not set when `post_message` returns; a `call_out` at t+4 confirms it appears once the continuation has run. This is a cross-operation deferred *operation*, not atomic event notification -- it does not share the triggering write's atomic envelope. It exercises the substrate's compiled-observer continuation (a `$delay` inside a dispatcher-fired observer; see `docs/dispatcher.md`, "Deferred observer continuations"), with the room exposing the standard `delayed_call` / `perform_delayed_call` glue. The observer stays pure Merry.

```text
ChatApp:test: DEFERRED-OP OK
```

## Multi-agent coherence

The load-bearing guarantee is write coherence: when two users contend for the same shared state, the runtime serializes the writes so exactly one succeeds and the other observes the committed result. The platform provides this without locks or a coordination protocol -- it falls out of reading current state at write time inside the runtime's single-threaded atomic-task scheduling.

### Phase 9 -- write coherence under contention

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

The phase sets a room's capacity to 1, then has two users both call `claim_slot`. Each call is its own atomic DGD task, so the runtime runs them one after another: the first reads zero members, takes the slot, and commits; the second reads the post-commit membership (one member, at capacity) and is refused. The driver asserts exactly one claim returned 1, the room holds exactly the winner, and the loser is not a member (sentinel `COHERENCE-SERIALIZE OK`). There is no lock and no retry loop -- the second writer's read at write time already reflects the first writer's commit, which is the coherent-state primitive doing the serialization.

The negative makes the contrast concrete. `claim_slot_stale` writes the member list from a snapshot captured *before* the other writer committed:

```c
void claim_slot_stale(object user, object *stale_snapshot)
{
    set_property("chat-room.member-list", stale_snapshot + ({ user }));
}
```

Phase 9b captures one empty snapshot and passes it to both writers. The second write (`stale_snapshot + ({ user })`) overwrites the first, so one writer's addition is lost and only the other remains (sentinel `LOST-UPDATE OK`). This is the lost update that appears when a writer acts on state read before a concurrent commit instead of re-reading at write time -- exactly the bug an external coordination layer would otherwise have to prevent, and the bug `claim_slot` avoids by construction.

The full agent-to-agent conflict scenario -- agents with identity wrappers contending through an inter-agent protocol -- is a downstream concern (the agent-identity workstream). This phase demonstrates the substrate serialization that such a wrapper exposes at the agent-facing surface.

### Phase 9c -- read coherence and the cached-snapshot drift

Write coherence's read-side companion: when all readers read the same runtime property, they see the same state at the same time. Three users in one room each read the message log after three messages have posted. The log is one property on the room clone, not a per-user copy, so the three reads return identical content in identical order -- the readers reconcile nothing (sentinel `COHERENCE OK`).

The negative shows where divergence would come from: an application that caches state the runtime already keeps coherent. One user captures the log, a fourth message posts, and the cached copy now disagrees with the live log -- three entries against four (sentinel `CACHED-DIVERGE OK`). This is the read-side analogue of the phase-9b lost update: the divergence is the cost of holding a private snapshot instead of reading the shared property at use time.

### Phase 9d -- atomic cross-room writes

The atomic primitive composes with coherent multi-agent state across more than one object. `cross_write` appends one message to two separate rooms:

```c
void cross_write(object room_a, object room_b, string content)
{
    mapping msg;

    msg = ([ "content": content, "timestamp": time() ]);
    room_a->set_property("chat-room.message-log",
			 room_a->query_messages() + ({ msg }));
    room_b->set_property("chat-room.message-log",
			 room_b->query_messages() + ({ msg }));
}
```

Run through `MERRY->batch(this_object(), "cross_write", args, ([ "atomic": 1 ]))`, the two appends are one atomic unit: the commit path lands both rooms (sentinel `ATOMIC-COMMIT OK`). A sibling `cross_write_then_fail` appends to the first room and then throws; under the atomic batch the throw rolls both writes back, so neither room retains the message (sentinel `ATOMIC-ROLLBACK OK`). This extends phase 7's single-room rollback across two room objects with no application-level transaction code.

The negative is the same failing writer run through a NON-atomic batch (`([ ])`, no `atomic` opt). Without the envelope the first room's append commits before the throw and survives, while the second room is never written -- partial state on error (sentinel `PARTIAL-STATE OK`). The all-or-nothing boundary the atomic batch provides is exactly what the non-atomic path leaves to the application to clean up.

### Phase 9e -- ancestry-walk multi-agent observer

UrHierarchy makes coherent reactive behavior something the platform provides to a cohort once, rather than something each member arranges. A base room carries the append observer; three child rooms set the base as their ur-object (`set_ur_object`) but register nothing themselves. A message posting in each child writes `chat-room.message`, and the dispatcher's ancestry walk resolves the base's observer -- with `$this` bound to the child, so the append lands on the child's own log. All three children record their message from the single ancestor registration (sentinel `ANCESTRY OK`). This re-exercises the dispatcher ancestry walk (cf. the merry-app DISPATCH ANCESTRY phase) at the application tier.

The negative is a room with neither its own observer nor an ancestor carrying one. Its message posts, the ancestry walk finds no append observer at any level, and nothing records it -- the log stays empty (sentinel `NO-INHERIT OK`). Absent the inherited registration, every room would need its own: the inheritance is what lets one registration serve the cohort coherently.

## What this example does not exercise

The chat-app reference is the multi-user messaging surface, not a MUD. The following adjacent concerns are deliberately out of scope:

- **Transport layer**: no telnet listener, no HTTP route handler, no WebSocket upgrade path. The test driver `IS` the client -- it simulates user sessions via direct LFUN calls. A future revision may add HTTP/WebSocket transport on top.
- **Multi-realm presence**: rooms and users live in a single realm. Presence is per-room, not per-realm-per-room.
- **Game content**: no SAM markup, no body/avatar layer, no light/bulk/proximity machinery, no stance/move/social verbs, no theme system, no voice/video chrome.
- **Message retention policies**: messages append to `chat-room.message-log` without bound. A real deployment would enforce per-room retention via a policy daemon.
- **Per-message edit/delete**: messages are append-only in the example. Edit/delete would arrive as additional admin verbs gated by per-room capability.

## Where to next

- `examples/chat-app/sys/test.c` -- the canonical boot-time test driver.
- `docs/dispatcher.md` -- the property-change dispatcher (referenced by sandboxed-reaction and async-event phases).
- `docs/persistence.md` -- the durable-state primitives.
- `docs/architecture.md` -- the capability tiers and daemon contracts that frame this application.
- `docs/runtime-primitives.md` -- the runtime primitives the chat application exercises.
