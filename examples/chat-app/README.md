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

## Deployment

Copy the directory into the kernel layer's `src/usr/` (`Chat` is the example's choice; pick any `/usr/<Name>/` that doesn't conflict with an existing domain):

```sh
cp -R examples/chat-app src/usr/Chat
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. Cross-domain calls into Merry, Vault, Schema, and Index are covered by the global-access grants in `src/usr/System/initd.c`.

## Verify

```sh
rm -f .runtime/state/snapshot* .runtime/state/swap
scripts/setup-runtime.sh
cp -R examples/chat-app .runtime/src/usr/Chat
.runtime/bin/dgd mva.dgd > /tmp/chat-app-boot.log 2>&1 &
sleep 3
cat .runtime/src/usr/Chat/data/test-result.log
kill %1
```

At this revision the expected result-log contents:

```text
ChatApp:test: starting
ChatApp:test: CAP-REJECT OK
ChatApp:test: CAP-ACCEPT OK
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
   +- test.c            -- boot-time test driver (capability phases wired)
```
