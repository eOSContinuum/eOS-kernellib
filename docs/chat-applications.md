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

A future revision demonstrates sandboxed code load by registering a Merry script as a per-room reaction on the `chat-room.message-log` property. An in-sandbox script (Merry source that calls only kfuns inside Merry's deny-list permission set) compiles and fires on each message post. A sibling out-of-sandbox script (Merry source that calls a forbidden kfun like `write_file`) compiles successfully but errors on its first fire because the SANDBOX shadow inside `merrynode.c` raises a "function not allowed in merry code" error.

The walkthrough for this section grows when the sandboxed-reaction phases land.

## Async events

A future revision demonstrates async event delivery by routing a `post_message` through the property-change dispatcher: the main timing fires the room's append-to-log observer, the post timing fires a mention-scan observer that walks the message content, and the mention-scan observer fires a cross-user notification observer on each mentioned user's mention-tracker property. The `[task-id]` annotation in the boot.log surfaces the async decoupling -- the caller's task returns before the post-timing observers fire.

The walkthrough for this section grows when the async-event phases land.

## Multi-agent coherence

A future revision demonstrates multi-agent coherence in three sub-phases. Multi-user same-room ordering: three users in one room each query `query_messages_since(t)` and observe identical message orderings. Atomic cross-room writes: `MERRY->batch(fn, atomic: true)` wraps three writes (append to source room, append to target user's mention-tracker, fire notification observer); a deliberate-failure variant rolls all three back. Ancestry-walk multi-agent: a room-base observer registered on the Room ancestor fires for all three Room clones.

The walkthrough for this section grows when the multi-agent-coherence phases land.

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
