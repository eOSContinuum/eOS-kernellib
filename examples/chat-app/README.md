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
- **Phase 10 (PERSIST-SETUP)**: the driver establishes a chat session on a fresh room -- two users join, exchange three messages, and one user's `chat-user.mention-tracker` is set to a cross-clone reference to the other user's object. The room and the two accounts are saved as object globals, the session shape is recorded to an on-disk marker, a verify `call_out` is scheduled, and the driver dumps a snapshot and exits via `/usr/System/sys/persist_helper::trigger_dump_and_exit`. Follows the two-boot pattern of `examples/merry-app/sys/test.c` phases 16/17.
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

# Boot 1 (cold): phases 1, 2, 10 run; phase 10 dumps a snapshot and the
# driver exits on its own.
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

Expected result-log contents (boot 1 writes the first four lines and
exits; boot 2 appends PERSIST-VERIFY OK; boot 3 appends COLDBOOT-LOST OK):

```text
ChatApp:test: starting
ChatApp:test: CAP-REJECT OK
ChatApp:test: CAP-ACCEPT OK
ChatApp:test: PERSIST-SETUP OK
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
   +- test.c            -- boot-time test driver (capability + persistence phases wired)
```
