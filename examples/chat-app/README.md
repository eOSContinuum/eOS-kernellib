# Reference Chat application

A minimal multi-user chat application that runs on top of eOS-kernellib. The runtime primitives the platform provides (property storage, ur-hierarchy, capability separation, the Merry dispatcher, orthogonal persistence) are exercised in turn by a boot-time test driver. Each phase produces a sentinel line in the result log so the smoke harness can verify the expected outcomes ran. The full walkthrough lives in `docs/chat-applications.md`.

The example is grown incrementally: the first revision wires the capability-separation phases; subsequent revisions add persistence, sandboxed reactions, async events, and multi-agent coherence. At any point in the growth the existing phases continue to pass; the smoke harness counts " OK" sentinels.

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
- **Phase 10 (PERSIST-SETUP)**: the driver establishes a chat session on a fresh room -- two users join, exchange three messages (recorded by an append observer registered on the room), and one user's `chat-user.mention-tracker` is set to a cross-clone reference to the other user's object. The room and the two accounts are saved as object globals, the session shape is recorded to an on-disk marker, and the boot-1 finalization schedules an async-verify `call_out` (t+4), a snapshot dump (t+5, via `/usr/System/sys/persist_helper::trigger_dump_and_exit`), and the post-restore persist-verify `call_out` (t+8, captured pending in the snapshot). Follows the two-boot pattern of `examples/merry-app/sys/test.c` phases 16/17.
- **Phase 11 (PERSIST-VERIFY)**: after the smoke harness restarts DGD against the snapshot, the surviving `call_out` fires. It asserts that all three messages, both member accounts (live clones with intact names), and the cross-clone mention-tracker reference (the same object, not a copy) survived the snapshot cycle, and that a third user joining the restored room observes the full prior session.
- **Negative case (COLDBOOT-LOST)**: a third boot starts DGD cold WITHOUT loading the snapshot. The on-disk marker (a file) survived, but the in-memory chat session did not -- the saved-as-global room is `nil` on a cold boot. This demonstrates that in-memory-only state does not survive a cold boot without a snapshot; only the on-disk record persists. Durability of structured state *beyond* the image snapshot (surviving a from-scratch boot) is the on-disk Schema/Marshal path that `examples/vault-app/` demonstrates.

## Deployment

Copy the directory into the kernel layer's `src/usr/` (`Chat` is the example's choice; pick any `/usr/<Name>/` that doesn't conflict with an existing domain):

```sh
cp -R examples/chat-app src/usr/Chat
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. Cross-domain calls into Merry, Vault, Schema, and Index are covered by the global-access grants in `src/usr/System/initd.c`.

## Verify

The capability phases run in a single cold boot; the persistence phases
span three boots (cold setup, restore, cold-no-snapshot). The marker and
result files live in the source tree, so clear the Chat domain before a
fresh run.

```sh
# Clean slate (the marker file lives under the deployed domain).
rm -rf .runtime/src/usr/Chat
rm -f .runtime/state/snapshot* .runtime/state/swap
scripts/setup-runtime.sh
cp -R examples/chat-app .runtime/src/usr/Chat

# Boot 1 (cold): phases 1-9b and 10 run; phase 4 logs a [caught]
# sandbox-rejection error to the boot log; phase 8 asserts from a t+4
# call_out (after its $delay continuation); a t+5 call_out then dumps a
# snapshot and the driver exits on its own.
.runtime/bin/dgd mva.dgd

# Boot 2 (restore): restart against the snapshot; phase 11 fires from the
# surviving call_out and appends PERSIST-VERIFY OK.
.runtime/bin/dgd mva.dgd .runtime/state/snapshot &
sleep 5
kill %1

# Boot 3 (cold, NO snapshot): the negative case. The on-disk marker
# survived but the in-memory session did not; appends COLDBOOT-LOST OK.
.runtime/bin/dgd mva.dgd &
sleep 3
kill %1

cat .runtime/src/usr/Chat/data/test-result.log
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
ChatApp:test: PERSIST-SETUP OK
ChatApp:test: DEFERRED-OP OK
ChatApp:test: PERSIST-VERIFY OK
ChatApp:test: COLDBOOT-LOST OK
```

Additional phases land as subsequent revisions add primitives.

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
