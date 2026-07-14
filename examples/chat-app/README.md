# Reference Chat application

A minimal multi-user chat application that runs on top of eOS-kernellib. The runtime primitives the platform provides (property storage, ur-hierarchy, capability separation, the Merry dispatcher, orthogonal persistence) are exercised in turn by a boot-time test driver. Each phase produces a sentinel line in the result log so the Verify procedure below can confirm the expected outcomes ran. The full walkthrough lives in `docs/chat-applications.md`.

The example is grown incrementally: the first revision wires the capability-separation phases; subsequent revisions add persistence, sandboxed reactions, async events, and multi-agent coherence. At any point in the growth the existing phases continue to pass; verification counts " OK" sentinel lines.

## Operations

- A `ChatApp:Room` clonable inherits `/lib/util/properties` (Vault-style property storage), `/lib/util/ur` (ur-parent / ur-child tracking; per-room Merry scripts ride this surface in later revisions), and `/lib/util/named` (logical-name registration via Index). It holds `chat-room.member-list`, `chat-room.message-log`, `chat-room.config`, and (when populated) `chat-room.banned`.
- A `ChatApp:User` clonable inherits the same three libs. It holds `chat-user.name`, `chat-user.room-subscriptions`, `chat-user.presence`, `chat-user.mention-tracker`, and the capability-bearing `chat-user.admin-tokens` list.
- An `admin_token` LWO under `/usr/Chat/data/admin_token` carries (grantor, subject, room, granted_at, actions). The capability check inside `sys/admin` walks the actor's `chat-user.admin-tokens` list and matches by room + action. Tokens are issued by `sys/admin::grant_admin` and attached to the subject's user object.
- The boot-time test driver `sys/test.c` runs in an order-independent fashion via a `call_out` from `create()`. Phases live in `run_tests`; each is wrapped in `catch{}` so a failure in one does not mask a different failure in another.
- **Phase 1 (CAP-REJECT)**: a regular user (no admin token) calls `admin->kick`. The capability check throws; the test driver records the rejection and asserts the target user's membership did not change.
- **Phase 2 (CAP-ACCEPT)**: a third user receives a per-room "kick" admin token via `admin->grant_admin(nil, carol, room_a, ({ "kick" }))`. The same `admin->kick` call now succeeds; the target is removed from the room's member list and the room is removed from the target's subscription list.
- **Phase 3 (SANDBOX-ACCEPT)**: two in-sandbox observers (append-to-history and a message-count marker) are registered on the room's `chat-room.message` arrival signal at main timing. Their sources call only unshadowed merryfuns (`Set` / `Get` / `sizeof`). The driver posts a message; `post_message` writes `chat-room.message`, and the dispatcher fires both observers automatically (no manual `run_merry`). The driver asserts the appended log and the `chat-room.message-count` marker landed -- sandboxed code load *and* dispatcher auto-fire demonstrated together.
- **Phase 4 (SANDBOX-REJECT)**: a sibling reaction whose source calls `write_file` -- a kfun in the `SANDBOX()` deny list -- is loaded on a distinct property path. Registration (compile) SUCCEEDS, because the Merry compiler resolves `write_file` to the local `SANDBOX(write_file)` shadow method, which exists. The first invocation errors with `function 'write_file' not allowed in merry code` (logged `[caught]`); the driver asserts the throw fired and that no file was created. The deny-by-shadow model is implicit-whitelist-by-presence: a kfun is callable from Merry only if no `SANDBOX()` shadow masks it.
- **Phase 5 (EVENT-ATOMIC)**: a room carries the append observer plus a post-timing mention-notify observer. alice posts a message mentioning dave; `post_message` resolves `@dave` into the message's `mentions` list. The post observer fires synchronously, within the same `set_property` that wrote `chat-room.message`, and writes dave's `chat-user.mention-tracker`. The driver asserts the tracker already names the room the instant `post_message` returns -- event notification atomic with state change. The sender carries no notification logic; the "asynchronous" part is the agent-side experience (it registered a listener, it did not poll).
- **Phase 6 (NO-REACT)**: the same mention post on a room with the append observer but no mention-notify observer. The dispatcher fires nothing at post timing, so the target's mention-tracker stays empty -- reactions happen only where observers are registered.
- **Phase 7 (EVENT-ROLLBACK)**: `atomic_post_then_fail` posts a message mentioning eve inside a DGD `atomic` function, then throws. The rollback unwinds every write the task made -- the message-log append and the synchronous mention-notify observer's cross-user Set alike. The driver asserts neither survived: the event is atomic with the state change, so when the change rolls back the event did not fire. (A queued or `$delay`-deferred notification would have escaped the envelope and fired anyway.)
- **Phase 8 (DEFERRED-OP)**: a distinct capability, kept separate so it is not conflated with phase 5's atomic notification. A post observer schedules a `$delay()` continuation that writes a delivery-receipt marker on a later tick. The driver asserts the receipt is *not* set when `post_message` returns, and a t+4 `call_out` confirms it appears once the continuation has run -- a cross-operation deferred operation that does not share the triggering write's atomic envelope. Exercises the substrate's compiled-observer continuation (`docs/dispatcher.md`).
- **Phase 9 (COHERENCE-SERIALIZE)**: two users contend for the single open slot in a capacity-1 room. Each `claim_slot` re-reads the room's CURRENT member list at write time, and each call is its own atomic task, so the two claims serialize: the first commits and fills the room; the second observes the post-commit membership and is refused. The driver asserts exactly one claim won, the room holds exactly the winner, and the loser is not a member. No lock and no coordination protocol -- the runtime's coherent-state read at write time is the serialization.
- **Phase 9b (LOST-UPDATE)**: the failure mode the coherent-state read removes, shown by contrast. Both writers call `claim_slot_stale` with the SAME member-list snapshot, captured before either committed; the second write overwrites the first, so one writer's claim is lost and only the other remains. The driver asserts the lost update appeared. Real code uses `claim_slot` (current read at write time); `claim_slot_stale` exists only to make the bug an external coordination protocol would otherwise have to prevent visible.
- **Phase 9c (COHERENCE / CACHED-DIVERGE)**: read coherence. Three users in one room each read the message log after three messages post; because the log is one runtime property rather than a per-user cached copy, all three reads return identical content in identical order. The CACHED-DIVERGE half is the read-side analogue of the lost update: one user caches the log, a fourth message posts, and the cached copy now disagrees with the live shared log -- the cost of caching state the runtime already keeps coherent.
- **Phase 9d (ATOMIC-COMMIT / ATOMIC-ROLLBACK / PARTIAL-STATE)**: atomic cross-room writes through the Merry batch surface. `cross_write` appends one message to two rooms; wrapped in `MERRY->batch(..., atomic: 1)` the two appends are one atomic unit, so the commit path lands both and the deliberate-failure path rolls both back -- cross-room, where phase 7 showed single-room. PARTIAL-STATE runs the same failing writer through a NON-atomic batch: without the envelope the first room's append survives the throw while the second is never written, the partial state the atomic boundary removes.
- **Phase 9e (ANCESTRY / NO-INHERIT)**: one observer registered on a room ancestor fans out to every room in the cohort via the UrHierarchy walk. A base room carries the append observer; three child rooms set the base as their ur-object but register nothing themselves, and a message posting in each child resolves the base's observer through the ancestry walk. The coherent reactive behavior is provided once, at the ancestor, to every cohort member. NO-INHERIT is the contrast: a room with neither its own observer nor an ancestor carrying one records nothing -- absent inheritance, each room would need its own registration.
- **Phase 10 (PERSIST-SETUP)**: the driver establishes a chat session on a fresh room -- two users join, exchange three messages (recorded by an append observer registered on the room), and one user's `chat-user.mention-tracker` is set to a cross-clone reference to the other user's object. The room and the two accounts are saved as object globals, the session shape is recorded to an on-disk marker, and the boot-1 finalization schedules an async-verify `call_out` (t+4), a snapshot dump (t+5, via `/usr/System/sys/persist_helper::trigger_dump_and_exit`), and the post-restore persist-verify `call_out` (t+8, captured pending in the snapshot). Follows the two-boot pattern of `examples/merry-app/sys/test.c` phases 16/17.
- **Phase 11 (PERSIST-VERIFY)**: after DGD restarts against the snapshot (boot 2 of the Verify procedure), the surviving `call_out` fires. It asserts that all three messages, both member accounts (live clones with intact names), and the cross-clone mention-tracker reference (the same object, not a copy) survived the snapshot cycle, and that a third user joining the restored room observes the full prior session.
- **Negative case (COLDBOOT-LOST)**: a third boot starts DGD cold WITHOUT loading the snapshot. The on-disk marker (a file) survived, but the in-memory chat session did not -- the saved-as-global room is `nil` on a cold boot. This demonstrates that in-memory-only state does not survive a cold boot without a snapshot; only the on-disk record persists. Durability of structured state *beyond* the image snapshot (surviving a from-scratch boot) is the on-disk Schema/Marshal path that `examples/vault-app/` demonstrates.

## Deployment

Copy the directory into the kernel layer's `src/usr/` (`Chat` is the example's choice; pick any `/usr/<Name>/` that doesn't conflict with an existing domain):

```sh
cp -R examples/chat-app src/usr/Chat
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. Cross-domain calls into Merry, Vault, Schema, and Index are covered by the global-access grants in `src/usr/System/initd.c`.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh chat-app
```

The runner does the clean-slate deploy, drives all three boots, and
asserts the sentinel count; boot output lands under `state/`.

The manual sequence it automates: the capability phases run in a single
cold boot; the persistence phases span three boots (cold setup, restore,
cold-no-snapshot). The marker and result files live in the source tree,
so clear the Chat domain before a fresh run.

```sh
# Clean slate (the marker file lives under the deployed domain).
rm -rf src/usr/Chat
rm -f state/snapshot* state/swap
cp -R examples/chat-app src/usr/Chat

# Boot 1 (cold): phases 1 through 9e and 10 run; phase 4 logs a [caught]
# sandbox-rejection error to the boot log; phase 8 asserts from a t+4
# call_out (after its $delay continuation); a t+5 call_out then dumps a
# snapshot and the driver exits on its own.
/path/to/dgd/bin/dgd example.dgd

# Boot 2 (restore): restart against the snapshot; phase 11 fires from the
# surviving call_out and appends PERSIST-VERIFY OK.
/path/to/dgd/bin/dgd example.dgd state/snapshot &
sleep 5
kill %1

# Boot 3 (cold, NO snapshot): the negative case. The on-disk marker
# survived but the in-memory session did not; appends COLDBOOT-LOST OK.
/path/to/dgd/bin/dgd example.dgd &
sleep 3
kill %1

cat src/usr/Chat/data/test-result.log
```

Expected result-log contents (boot 1 writes through DEFERRED-OP OK and
exits; boot 2 appends PERSIST-VERIFY OK; boot 3 appends COLDBOOT-LOST OK.
DEFERRED-OP OK lands after PERSIST-SETUP OK because it fires from the t+4
call_out, while the earlier phases assert inline):

```text
ChatApp:test: starting
ChatApp:test: CAP-REJECT OK
ChatApp:test: CAP-ACCEPT OK
ChatApp:test: SANDBOX-ACCEPT OK
ChatApp:test: SANDBOX-REJECT OK
ChatApp:test: EVENT-ATOMIC OK
ChatApp:test: NO-REACT OK
ChatApp:test: EVENT-ROLLBACK OK
ChatApp:test: COHERENCE-SERIALIZE OK
ChatApp:test: LOST-UPDATE OK
ChatApp:test: COHERENCE OK
ChatApp:test: CACHED-DIVERGE OK
ChatApp:test: ATOMIC-COMMIT OK
ChatApp:test: ATOMIC-ROLLBACK OK
ChatApp:test: PARTIAL-STATE OK
ChatApp:test: ANCESTRY OK
ChatApp:test: NO-INHERIT OK
ChatApp:test: PERSIST-SETUP OK
ChatApp:test: DEFERRED-OP OK
ChatApp:test: PERSIST-VERIFY OK
ChatApp:test: COLDBOOT-LOST OK
```

Additional phases land as subsequent revisions add primitives.

## Phase notes

The phase-by-phase account of what the driver (`sys/test.c`) does and
asserts. The build patterns these phases prove are documented in
`../../docs/chat-applications.md`; this section is the evidence tour.

### Phase 1 -- rejected kick (`CAP-REJECT OK`)

The driver clones three users (alice, bob, carol) and one room. None of
them carry an admin token at this point. bob calls
`admin->kick(alice, room_a)`; the capability check finds no matching
token and throws. The driver's `catch{}` converts the throw into the
sentinel and asserts that the membership of `room_a` did not mutate.

The boot log carries the `[caught]` annotation that DGD writes for any
error-that-was-caught:

```text
** admin: actor bob not authorized for kick in room ChatApp:Room:LobbyA [caught]
                       /usr/Chat/sys/test
  202   setup_and_run         /usr/Chat/sys/test
  240 * run_tests             /usr/Chat/sys/test
                       /usr/Chat/sys/admin
   53   kick                  /usr/Chat/sys/admin
  149   _check_admin_token    /usr/Chat/sys/admin
```

The `[caught]` suffix confirms the error did not propagate to the
runtime -- the driver's `catch{}` swallowed it. DGD's convention is to
log every error regardless of `catch{}` so the platform retains a
trace; the caught/uncaught distinction is in the suffix.

### Phase 2 -- accepted kick (`CAP-ACCEPT OK`)

carol receives a per-room `"kick"` token via
`admin->grant_admin(nil, carol, room_a, ({ "kick" }))`. The same
`admin->kick(carol, alice, room_a)` call now passes the capability
check; the verb removes alice from `room_a`'s member list and removes
`room_a` from alice's subscription list. The driver asserts both
removals.

### Phase 3 -- accepted reaction (`SANDBOX-ACCEPT OK`)

The driver registers two in-sandbox observers on the room's
`chat-room.message` arrival signal at main timing, each calling only
unshadowed merryfuns (`Set` / `Get` / `sizeof`, all routing through
`merrynode.c`) -- an append-to-history observer and a message-count
marker observer:

```c
MERRY->register_observer(room_a, "chat-room.message", "main",
    "Set($this, \"chat-room.message-log\", " +
    "Get($this, \"chat-room.message-log\") + ({ $new })); return TRUE;");
MERRY->register_observer(room_a, "chat-room.message", "main",
    "Set($this, \"chat-room.message-count\", " +
    "sizeof(Get($this, \"chat-room.message-log\"))); return TRUE;");
```

Registration order is firing order, so the marker observes the
already-appended log. bob posts a message; `post_message` writes
`chat-room.message`, and the dispatcher fires both observers with no
manual `run_merry`. The driver reads the appended log and the marker
back. This is sandboxed code load *as a registered reaction*: scripts
authored as strings, compiled at runtime, bound on a room, and executed
against that room's property storage automatically on a real state
change.

### Phase 4 -- rejected reaction (`SANDBOX-REJECT OK`)

The driver compiles a sibling reaction whose source calls `write_file`,
a kfun in the `SANDBOX()` deny list:

```c
write_file("/usr/Chat/data/hack.txt", "pwned");
```

Registration succeeds: the Merry compiler resolves `write_file` to the
local `SANDBOX(write_file)` shadow method, which exists, so compilation
finds the symbol and binds the script. The error fires only on the
first invocation, when the shadow body runs and raises:

```text
** function 'write_file' not allowed in merry code [caught]
```

The boot log carries the full call stack down to `merrynode.c`'s
`SANDBOX(write_file)` shadow. The driver's `catch{}` converts the throw
into the sentinel and asserts that no `hack.txt` was created.

### Phase 5 -- synchronous cross-user notification (`EVENT-ATOMIC OK`)

A room carries an append-to-history main observer and a mention-notify
post observer. alice posts a message mentioning dave; `scan_mentions`
resolves `@dave` and carries him in the message's `mentions` list. The
dispatcher fires the main append, then the post observer -- both
synchronously:

```c
Set($new["mentions"][0], "chat-user.mention-tracker",
    Get($new["mentions"][0], "chat-user.mention-tracker") + ({ $this }));
return TRUE;
```

The instant `post_message` returns, dave's mention-tracker already
names the room -- the notification landed inside the same
`set_property` that wrote `chat-room.message`. `post_message` itself
contains no notification logic.

### Phase 6 -- no observer, no reaction (`NO-REACT OK`)

The same mention post, on a room with the append observer but no
mention-notify observer registered. The dispatcher has nothing to fire
at post timing, so the target's mention-tracker stays empty. The
negative isolates this to the missing post observer: the append
observer is registered, so the message is still recorded.

### Phase 7 -- event atomic with state change (`EVENT-ROLLBACK OK`)

`atomic_post_then_fail` posts a message mentioning eve inside a DGD
`atomic` function, then throws. The rollback unwinds every write the
task made -- the message-log append and the observer's cross-user
mention-tracker Set. After the caught throw, the room has no message
and eve has no notification: the event "did not fire" in the same sense
the state change "did not occur".

### Phase 8 -- a deferred operation, kept distinct (`DEFERRED-OP OK`)

A post observer schedules a `$delay()` continuation that writes a
delivery-receipt marker on a subsequent tick:

```c
$delay(2, FALSE);
Set($this, "chat-room.delivery-receipt", 1);
return TRUE;
```

`$delay(2, FALSE)` returns `FALSE` to the dispatcher immediately, so
the receipt is not set when `post_message` returns; a `call_out` at t+4
confirms it appears once the continuation has run -- a cross-operation
deferred operation that does not share the triggering write's atomic
envelope, exercising the compiled-observer continuation with the room
exposing the standard `delayed_call` / `perform_delayed_call` glue.

### Phase 9 -- write coherence under contention (`COHERENCE-SERIALIZE OK`)

The phase sets a room's capacity to 1, then has two users both call
`claim_slot`. Each call is its own atomic DGD task, so the runtime runs
them one after another: the first reads zero members, takes the slot,
and commits; the second reads the post-commit membership (one member,
at capacity) and is refused. The driver asserts exactly one claim
returned 1, the room holds exactly the winner, and the loser is not a
member.

### Phase 9b -- the lost update (`LOST-UPDATE OK`)

`claim_slot_stale` writes the member list from a snapshot captured
before the other writer committed:

```c
void claim_slot_stale(object user, object *stale_snapshot)
{
    set_property("chat-room.member-list", stale_snapshot + ({ user }));
}
```

The phase captures one empty snapshot and passes it to both writers.
The second write (`stale_snapshot + ({ user })`) overwrites the first,
so one writer's addition is lost and only the other remains -- exactly
the bug an external coordination layer would otherwise have to prevent,
and the bug `claim_slot` avoids by construction.

### Phase 9c -- read coherence and cached drift (`COHERENCE OK`, `CACHED-DIVERGE OK`)

Three users in one room each read the message log after three messages
have posted. The log is one property on the room clone, not a per-user
copy, so the three reads return identical content in identical order --
the readers reconcile nothing. The negative: one user captures the log,
a fourth message posts, and the cached copy now disagrees with the live
log -- three entries against four. The divergence is the cost of
holding a private snapshot instead of reading the shared property at
use time.

### Phase 9d -- atomic cross-room writes (`ATOMIC-COMMIT OK`, `ATOMIC-ROLLBACK OK`, `PARTIAL-STATE OK`)

`cross_write` appends one message to two separate rooms:

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

Run through `MERRY->batch(this_object(), "cross_write", args,
([ "atomic": 1 ]))`, the two appends are one atomic unit: the commit
path lands both rooms. A sibling `cross_write_then_fail` appends to the
first room and then throws; under the atomic batch the throw rolls both
writes back, so neither room retains the message. The negative is the
same failing writer run through a non-atomic batch (`([ ])`): the first
room's append commits before the throw and survives, while the second
room is never written -- partial state on error.

### Phase 9e -- ancestry-walk multi-agent observer (`ANCESTRY OK`, `NO-INHERIT OK`)

A base room carries the append observer; three child rooms set the base
as their ur-object (`set_ur_object`) but register nothing themselves. A
message posting in each child writes `chat-room.message`, and the
dispatcher's ancestry walk resolves the base's observer -- with `$this`
bound to the child, so the append lands on the child's own log. All
three children record their message from the single ancestor
registration (cf. the merry-app DISPATCH ANCESTRY phase). The negative
is a room with neither its own observer nor an ancestor carrying one:
its message posts, the ancestry walk finds no append observer at any
level, and the log stays empty.

### Phase 10 -- persist setup (`PERSIST-SETUP OK`)

The driver clones a fresh room (`ChatApp:Room:PersistD`), has alice and
bob join it, and posts three messages through `chat::post_message`. It
then sets alice's `chat-user.mention-tracker` to `({ bob })` -- a
cross-clone object reference, alice's user object pointing directly at
bob's. The room and the two accounts are saved as object globals on the
test driver (non-`static` so the state dump captures them), the session
shape is recorded to an on-disk marker file, a `persist_verify`
`call_out` is scheduled at `t=3`, and the driver calls
`/usr/System/sys/persist_helper::trigger_dump_and_exit`. That helper
schedules `dump_state(FALSE)` + `shutdown()` on its own `call_out`, so
the caller's stack unwinds before the snapshot is taken.

The snapshot captures everything reachable in the image: the room
clone, the two user clones, the three message mappings (each carrying a
`sender` object reference), alice's cross-clone mention-tracker
reference, and the not-yet-fired `persist_verify` `call_out`. The
driver then exits; the boot log records `** System halted.`

### Phase 11 -- persist verify (`PERSIST-VERIFY OK`)

The second boot restarts DGD against the snapshot
(`dgd example.dgd state/snapshot`). DGD does not re-run `create()` on
restored objects; it resumes the dumped image and fires `call_out`s
whose scheduled times have elapsed. The surviving `persist_verify`
`call_out` fires immediately and asserts:

- the room still holds three messages and two members;
- `persist_alice` and `persist_bob` resolve to live clones with their
  `chat-user.name` values intact (the user accounts survived);
- alice's mention-tracker resurrected as the *same* bob object
  (`mention[0] == persist_bob`), not a copy -- DGD's object-reference
  resurrection rebound the cross-clone reference;
- a third user (carol) joining the restored room is admitted and
  observes the full prior session: three messages and a
  now-three-member roster.

### Negative case -- cold boot without a snapshot (`COLDBOOT-LOST OK`)

A third boot starts DGD cold without the snapshot argument. The
driver's `setup_and_run` finds the on-disk marker present (a file -- it
survives any boot) but `persist_room` `nil` (a cold boot resets all
object globals). That combination is the signature of "a session was
persisted but the snapshot was not loaded," so the driver writes the
negative sentinel rather than re-running setup.

## Layout

```text
examples/chat-app/
+- README.md            -- this file
+- initd.c              -- domain initd; compiles room/user/admin_token/chat/admin/test
+- lib/
|  +- app.c             -- marker lib (shared inherit chain for sys/*)
+- obj/
|  +- room.c            -- ChatApp:Room clonable (member list + message log + config)
|  +- user.c            -- ChatApp:User clonable (name + presence + admin tokens)
+- data/
|  +- admin_token.c     -- capability LWO (grantor + subject + room + actions)
+- sys/
   +- chat.c            -- chat dispatcher daemon (join_room / leave_room / post_message)
   +- admin.c           -- admin daemon (capability-gated kick / ban / set_room_config)
   +- test.c            -- boot-time test driver (capability + sandbox + persistence phases wired)
```
