/*
 * Boot-time test driver for the Chat application example.
 *
 * Exercises the runtime primitives via boot-time phases. Each phase
 * writes a sentinel line to the result file at
 * /usr/Chat/data/test-result.log on success; failures write a
 * distinct FAIL sentinel. Phases are wrapped in catch{} so a failure
 * in one does not mask a different failure in another. The driver
 * grows with each primitive added; the first revision wires the
 * capability-separation phases.
 *
 * Phases in this revision:
 *
 *   1.  CAP-REJECT OK      -- regular user calls admin->kick without
 *                             an admin-token; capability check throws.
 *   2.  CAP-ACCEPT OK      -- admin user holding a per-room kick
 *                             token calls admin->kick; target leaves
 *                             the room.
 *   3.  SANDBOX-ACCEPT OK  -- two sandboxed Merry observers (append-to-
 *                             history + a message-count marker) are
 *                             registered on the room's chat-room.message
 *                             arrival signal at main timing; a message
 *                             post fires them automatically via the
 *                             dispatcher (no manual run_merry) and the
 *                             marker lands -- sandboxed code load and
 *                             dispatcher auto-fire demonstrated together.
 *   4.  SANDBOX-REJECT OK  -- a sibling reaction whose source calls a
 *                             sandbox-denied kfun (write_file) compiles
 *                             (registration succeeds) but errors on its
 *                             first fire with the merrynode SANDBOX
 *                             shadow's "function not allowed in merry
 *                             code"; no file is created.
 *   5.  EVENT-ATOMIC OK    -- async events. A mention post fires a
 *                             post-timing mention-notify observer
 *                             synchronously, within the same write: the
 *                             target's mention-tracker is populated the
 *                             instant post_message returns. The sender
 *                             carries no notification logic -- the
 *                             "asynchronous" experience is agent-side;
 *                             the mechanism is synchronous-in-envelope.
 *   6.  NO-REACT OK        -- async-events negative. The same mention
 *                             post on a room with no mention-notify
 *                             observer fires no reaction -- reactions
 *                             happen only where observers are registered.
 *   7.  EVENT-ROLLBACK OK  -- event/state atomicity. A mention post
 *                             wrapped in an atomic envelope that throws
 *                             rolls BOTH back: neither the message nor
 *                             the notification lands. The event is atomic
 *                             with the state change -- if the change
 *                             rolls back, the event did not fire.
 *   8.  DEFERRED-OP OK     -- a distinct capability. A post observer
 *                             schedules a $delay() continuation that runs
 *                             on a later tick (a delivery receipt). This
 *                             is a deferred OPERATION, explicitly not the
 *                             atomic event notification of phase 5 --
 *                             surfacing the boundary between the two.
 *  10.  PERSIST-SETUP OK   -- establish a chat session (2 users, 3
 *                             messages via the append observer, a cross-
 *                             clone mention-tracker reference) on a fresh
 *                             room, record the session shape to an
 *                             on-disk marker, then dump a snapshot.
 *  11.  PERSIST-VERIFY OK  -- fires from a surviving call_out after the
 *                             snapshot is restored; the whole session
 *                             survived and a third user joining the
 *                             restored room observes all prior state.
 *      COLDBOOT-LOST OK    -- negative case. On a cold boot taken
 *                             WITHOUT loading the snapshot, the on-disk
 *                             marker survived but the in-memory session
 *                             did not; in-memory-only state does not
 *                             survive a cold boot without a snapshot.
 *
 * Phases 5-7 assert inline (the notification is synchronous, so it has
 * landed or rolled back by the time post_message returns). Phase 8's
 * deferred operation asserts from a t+4 call_out on boot 1, before the
 * t+5 snapshot dump. The persistence phases follow the two-boot pattern
 * of examples/merry-app/sys/test.c phases 16/17, plus a third (cold,
 * no-snapshot) boot for the negative case.
 *
 * Pass/fail is observable via the sentinel file. The README's verify
 * command counts " OK" lines after the smoke harness completes.
 *
 * Deferred to a call_out so System/initd's domain-load loop has run
 * to completion before any cross-domain call fires. The Chat domain
 * is alphabetically earlier than Merry, Vault, etc., so a direct
 * cross-domain call at create() time would hit not-yet-loaded
 * daemons. The call_out fires once every per-domain initd has
 * returned.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/Chat/lib/app";
inherit "/usr/System/lib/auto";
inherit "/lib/util/lpc";

/* Merry's invocation API is a static surface (find_merry / run_merry).
 * Inherited rather than called via the SYS_MERRY daemon so the static
 * methods resolve at compile time -- the convention merryapi.c was
 * authored against, mirrored by examples/merry-app/sys/test.c. The
 * cross-domain inherit + the cross-domain clone of ~Merry/data/merry
 * below ride the global-access grants set in src/usr/System/initd.c. */
inherit "/usr/Merry/lib/merryapi";

# define CHAT_DAEMON    "/usr/Chat/sys/chat"
# define ADMIN_DAEMON   "/usr/Chat/sys/admin"
# define ROOM_PROG      "/usr/Chat/obj/room"
# define USER_PROG      "/usr/Chat/obj/user"
# define RESULT_FILE    "/usr/Chat/data/test-result.log"
# define MARKER_FILE    "/usr/Chat/data/persist-marker.log"
# define PERSIST_HELPER "/usr/System/sys/persist_helper"
# define MERRY_DATA     "/usr/Merry/data/merry"
# define MERRY_DAEMON   "/usr/Merry/sys/merry"
# define HACK_FILE      "/usr/Chat/data/hack.txt"

/* Observer source strings, compiled into the merrynode sandbox by
 * register_observer. Each is a thin Merry reaction over the dispatcher's
 * change context ($this = dispatch host, $new = the value written). */

/* main timing: append the arriving message mapping to the room's log.
 * (LPC has no adjacent-string-literal concatenation, so the fragments
 * are joined with + the same way examples/merry-app/sys/test.c builds
 * its observer sources.) */
# define SRC_APPEND \
    "Set($this, \"chat-room.message-log\", " + \
    "Get($this, \"chat-room.message-log\") + ({ $new })); return TRUE;"

/* main timing: record the message count off the (now-appended) log --
 * the sandboxed-marker reaction, auto-fired by the dispatcher. */
# define SRC_MARKER \
    "Set($this, \"chat-room.message-count\", " + \
    "sizeof(Get($this, \"chat-room.message-log\"))); return TRUE;"

/* post timing: notify the first mentioned user synchronously, within the
 * same write that triggered the dispatch. The cross-user Set lands before
 * post_message returns -- event notification atomic with state change.
 * The sender carries no notification logic; the "asynchronous" part is
 * the agent-side experience (it registered a listener; it did not poll). */
# define SRC_NOTIFY \
    "Set($new[\"mentions\"][0], \"chat-user.mention-tracker\", " + \
    "Get($new[\"mentions\"][0], \"chat-user.mention-tracker\") + ({ $this })); " + \
    "return TRUE;"

/* post timing: a DEFERRED OPERATION, distinct from the synchronous event
 * notification above. $delay() returns control to the sender immediately
 * and resumes on a later tick to write a delivery-receipt marker. This is
 * not atomic event notification -- it is a cross-operation deferred call,
 * the boundary the platform asks consumers to keep distinct. It exercises
 * the substrate's compiled-observer continuation (a $delay inside a
 * dispatcher-fired observer; see docs/dispatcher.md). */
# define SRC_DEFERRED \
    "$delay(2, FALSE); " + \
    "Set($this, \"chat-room.delivery-receipt\", 1); return TRUE;"

static void run_tests();
static atomic void atomic_post_then_fail(object sender, object room, string content);
static void deferred_verify();
static void finalize_dump();
static void persist_verify();
private void log_line(string msg);
private object spawn_user(string name);
private object spawn_room(string id);
private int marker_present();
private void coldboot_loss_verify();

/* Phase 10/11 binding hosts. Non-static object variables so DGD's state
 * dump captures them; phase 11 (persist_verify) resolves the restored
 * session through these after the snapshot restore. */
object persist_room;    /* the chat-session room; survives the snapshot */
object persist_alice;   /* a member account; holds the mention-tracker  */
object persist_bob;     /* a member account; the mention-tracker target */

/* Phase 8 deferred-operation handle. Set during run_tests on boot 1 and
 * read by deferred_verify (call_out at t+4, before the snapshot dump) to
 * assert the $delay continuation wrote the delivery receipt on a later
 * tick. Phases 5-7 assert inline (their notification is synchronous), so
 * they need no global. */
object deferred_room;   /* phase 8 room: append + a $delay deferred observer */


static void create()
{
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    catch(make_dir("/usr/Chat/data"));

    /* Negative-case path. Reached only on a cold boot (no snapshot
     * loaded) that follows a boot which wrote the marker in phase 10:
     * the on-disk marker is present, but the in-image chat session it
     * recorded is gone (persist_room is a fresh nil global on a cold
     * boot). Append the COLDBOOT-LOST sentinel WITHOUT wiping the
     * boot-1 / boot-2 transcript, then stop -- do not re-run setup. */
    if (marker_present() && !persist_room) {
	coldboot_loss_verify();
	return;
    }

    catch(remove_file(RESULT_FILE));
    run_tests();
}


static void run_tests()
{
    object alice, bob, carol;
    object room_a;
    mixed token;
    int rejected;

    log_line("ChatApp:test: starting");

    /* shared setup: three users + one room. alice and bob are
     * regular users; carol receives a per-room "kick" admin token
     * later in phase 2. alice joins room_a so the kick has a target
     * to remove. */
    catch {
	alice = spawn_user("alice");
	bob   = spawn_user("bob");
	carol = spawn_user("carol");
	room_a = spawn_room("ChatApp:Room:LobbyA");
	CHAT_DAEMON->join_room(alice, room_a);
	CHAT_DAEMON->join_room(bob, room_a);
    } : {
	log_line("ChatApp:test: FAIL: shared setup threw");
	return;
    }

    /* phase 1: CAP-REJECT
     *
     * bob (no admin token) calls admin->kick. The capability check
     * walks bob's admin-tokens list, finds nothing matching room_a +
     * "kick", and throws. The catch{} below converts the throw into
     * a CAP-REJECT OK sentinel.
     */
    rejected = 0;
    catch {
	ADMIN_DAEMON->kick(bob, alice, room_a);
    } : {
	rejected = 1;
    }
    if (!rejected) {
	log_line("ChatApp:test: FAIL: CAP-REJECT kick did not throw");
	return;
    }
    /* The reject must not have removed alice from the room. */
    if (!member(alice, room_a->query_members())) {
	log_line("ChatApp:test: FAIL: CAP-REJECT membership mutated");
	return;
    }
    log_line("ChatApp:test: CAP-REJECT OK");

    /* phase 2: CAP-ACCEPT
     *
     * carol receives a per-room "kick" admin token. carol calls
     * admin->kick; the capability check matches and the verb removes
     * alice from room_a. The catch{} below records FAIL if anything
     * throws.
     */
    catch {
	token = ADMIN_DAEMON->grant_admin(nil, carol, room_a, ({ "kick" }));
    } : {
	log_line("ChatApp:test: FAIL: CAP-ACCEPT grant_admin threw");
	return;
    }
    if (!token) {
	log_line("ChatApp:test: FAIL: CAP-ACCEPT grant_admin returned nil");
	return;
    }

    catch {
	ADMIN_DAEMON->kick(carol, alice, room_a);
    } : {
	log_line("ChatApp:test: FAIL: CAP-ACCEPT kick threw");
	return;
    }
    if (member(alice, room_a->query_members())) {
	log_line("ChatApp:test: FAIL: CAP-ACCEPT alice still member");
	return;
    }
    if (member(room_a, alice->query_subscriptions())) {
	log_line("ChatApp:test: FAIL: CAP-ACCEPT subscription persisted");
	return;
    }
    log_line("ChatApp:test: CAP-ACCEPT OK");

    /* phase 3: SANDBOX-ACCEPT -- sandboxed reaction auto-fired by the
     * dispatcher (merged with the message-arrival path).
     *
     * Two sandboxed Merry observers are registered on room_a's
     * chat-room.message arrival signal at main timing: an append-to-
     * history observer (SRC_APPEND) and a message-count marker observer
     * (SRC_MARKER). Registration order is firing order, so the marker
     * observes the already-appended log. Both sources call only
     * in-sandbox merryfuns (Set / Get / sizeof) -- nothing in
     * merrynode.c's SANDBOX() deny list -- so they compile into the
     * sandbox and execute cleanly.
     *
     * bob posts a message (he is still a member after the phase-2 kick
     * removed alice); post_message writes chat-room.message; the
     * dispatcher fires both observers with NO manual run_merry. The
     * marker landing is the sandboxed-code-load claim (loaded Merry code
     * executes in the sandbox and mutates room state) AND the auto-fire claim (the
     * reaction fires automatically off a real property change),
     * demonstrated together. The assertions read the appended log and
     * the marker back.
     */
    catch {
	MERRY_DAEMON->register_observer(room_a, "chat-room.message", "main",
					SRC_APPEND);
	MERRY_DAEMON->register_observer(room_a, "chat-room.message", "main",
					SRC_MARKER);

	CHAT_DAEMON->post_message(bob, room_a, "first post");

	if (sizeof(room_a->query_messages()) != 1) {
	    log_line("ChatApp:test: FAIL: SANDBOX-ACCEPT append observer did not record message");
	    return;
	}
	if (room_a->query_raw_property("chat-room.message-count") != 1) {
	    log_line("ChatApp:test: FAIL: SANDBOX-ACCEPT marker property did not land");
	    return;
	}
    } : {
	log_line("ChatApp:test: FAIL: SANDBOX-ACCEPT threw");
	return;
    }
    log_line("ChatApp:test: SANDBOX-ACCEPT OK");

    /* phase 4: SANDBOX-REJECT
     *
     * Load a sibling reaction whose source calls write_file -- a kfun
     * in merrynode.c's SANDBOX() deny list. Compilation (registration)
     * SUCCEEDS: the Merry compiler resolves write_file to the local
     * SANDBOX(write_file) shadow method, which exists. The error fires
     * only on the FIRST invocation, when the shadow raises "function
     * 'write_file' not allowed in merry code". The deny-by-shadow
     * mechanism is the implicit-whitelist-by-presence model: a kfun is
     * callable from Merry only if no SANDBOX() shadow masks it.
     *
     * The compile is in its own catch{} (a throw there is a FAIL --
     * the registration is supposed to succeed). The fire is in a
     * separate catch{} that EXPECTS the throw; reaching the line after
     * run_merry without a throw is the FAIL. A final guard confirms
     * the forbidden write produced no file.
     */
    catch {
	object reject_script;

	reject_script = new_object(MERRY_DATA,
		"write_file(\"" + HACK_FILE + "\", \"pwned\");");
	room_a->set_property("merry:on:chat-room.intrusion:main",
			     reject_script);
    } : {
	log_line("ChatApp:test: FAIL: SANDBOX-REJECT compile/register threw");
	return;
    }

    catch(remove_file(HACK_FILE));
    rejected = 0;
    catch {
	run_merry(room_a, "chat-room.intrusion:main", "on", ([ ]));
    } : {
	rejected = 1;
    }
    if (!rejected) {
	log_line("ChatApp:test: FAIL: SANDBOX-REJECT denied kfun did not throw");
	return;
    }
    if (file_info(HACK_FILE)) {
	log_line("ChatApp:test: FAIL: SANDBOX-REJECT forbidden write created a file");
	return;
    }
    log_line("ChatApp:test: SANDBOX-REJECT OK");

    /* phase 5: EVENT-ATOMIC
     *
     * A fresh room with an append-to-history main observer AND a
     * mention-notify post observer. dave joins; alice posts a message
     * mentioning him. post_message resolves "@dave" against the roster and
     * carries dave in msg["mentions"]. The dispatcher fires the main
     * append, then the post mention-notify observer -- both synchronously,
     * inside the same set_property that wrote chat-room.message. So the
     * instant post_message returns, dave's mention-tracker already names
     * the room: the notification is atomic with the state change. The
     * sender (alice's post_message) carries no notification logic -- the
     * "asynchronous" experience is agent-side; the mechanism is
     * synchronous-within-the-write. */
    {
	object room_p5, dave;

	catch {
	    room_p5 = spawn_room("ChatApp:Room:EventP5");
	    dave    = spawn_user("dave");
	    CHAT_DAEMON->join_room(alice, room_p5);
	    CHAT_DAEMON->join_room(dave, room_p5);

	    MERRY_DAEMON->register_observer(room_p5, "chat-room.message", "main",
					    SRC_APPEND);
	    MERRY_DAEMON->register_observer(room_p5, "chat-room.message", "post",
					    SRC_NOTIFY);

	    CHAT_DAEMON->post_message(alice, room_p5, "@dave hi");

	    if (sizeof(dave->query_mention_tracker()) != 1 ||
		dave->query_mention_tracker()[0] != room_p5) {
		log_line("ChatApp:test: FAIL: EVENT-ATOMIC notification not landed synchronously");
		return;
	    }
	    if (sizeof(room_p5->query_messages()) != 1) {
		log_line("ChatApp:test: FAIL: EVENT-ATOMIC append observer did not record message");
		return;
	    }
	} : {
	    log_line("ChatApp:test: FAIL: EVENT-ATOMIC threw");
	    return;
	}
	log_line("ChatApp:test: EVENT-ATOMIC OK");
    }

    /* phase 6: NO-REACT (negative)
     *
     * The same mention post on a room with NO mention-notify observer.
     * Without a post-timing observer the dispatcher has nothing to fire
     * after the write, so no cross-user notification happens. The target's
     * mention-tracker stays empty -- a property write on an un-observed
     * timing slot is inert. The append observer IS registered, so the
     * message is still recorded -- isolating the negative to the missing
     * post observer, not a broken post path. */
    {
	object room_p6, dave2;

	catch {
	    room_p6 = spawn_room("ChatApp:Room:NoReactP6");
	    dave2   = spawn_user("dave2");
	    CHAT_DAEMON->join_room(bob, room_p6);
	    CHAT_DAEMON->join_room(dave2, room_p6);

	    MERRY_DAEMON->register_observer(room_p6, "chat-room.message", "main",
					    SRC_APPEND);
	    /* deliberately NO mention-notify post observer registered. */

	    CHAT_DAEMON->post_message(bob, room_p6, "@dave2 yo");

	    if (sizeof(dave2->query_mention_tracker()) != 0) {
		log_line("ChatApp:test: FAIL: NO-REACT notification fired without an observer");
		return;
	    }
	    if (sizeof(room_p6->query_messages()) != 1) {
		log_line("ChatApp:test: FAIL: NO-REACT append observer did not record message");
		return;
	    }
	} : {
	    log_line("ChatApp:test: FAIL: NO-REACT setup threw");
	    return;
	}
	log_line("ChatApp:test: NO-REACT OK");
    }

    /* phase 7: EVENT-ROLLBACK
     *
     * Event notification is atomic with the state change: if the change
     * rolls back, the event did not fire. eve joins a room with the same
     * append + synchronous mention-notify observers. atomic_post_then_fail
     * posts a message mentioning eve (firing both observers inside the
     * atomic envelope) and then throws. DGD's atomic{} rolls back EVERY
     * write the task made: the message-log append AND the observer's
     * cross-user mention-tracker Set. After the caught throw, neither
     * landed -- the room has no message and eve has no notification. A
     * queued / $delay-deferred notification would have escaped the
     * envelope and fired anyway; the synchronous-in-envelope notification
     * is what makes the rollback total. */
    {
	object room_p7, eve;
	int threw;

	catch {
	    room_p7 = spawn_room("ChatApp:Room:RollbackP7");
	    eve     = spawn_user("eve");
	    CHAT_DAEMON->join_room(alice, room_p7);
	    CHAT_DAEMON->join_room(eve, room_p7);

	    MERRY_DAEMON->register_observer(room_p7, "chat-room.message", "main",
					    SRC_APPEND);
	    MERRY_DAEMON->register_observer(room_p7, "chat-room.message", "post",
					    SRC_NOTIFY);
	} : {
	    log_line("ChatApp:test: FAIL: EVENT-ROLLBACK setup threw");
	    return;
	}

	threw = 0;
	catch {
	    atomic_post_then_fail(alice, room_p7, "@eve hi");
	} : {
	    threw = 1;
	}
	if (!threw) {
	    log_line("ChatApp:test: FAIL: EVENT-ROLLBACK atomic post did not throw");
	    return;
	}
	if (sizeof(room_p7->query_messages()) != 0) {
	    log_line("ChatApp:test: FAIL: EVENT-ROLLBACK message survived the rollback");
	    return;
	}
	if (sizeof(eve->query_mention_tracker()) != 0) {
	    log_line("ChatApp:test: FAIL: EVENT-ROLLBACK notification survived the rollback");
	    return;
	}
	log_line("ChatApp:test: EVENT-ROLLBACK OK");
    }

    /* phase 8: DEFERRED-OP -- setup.
     *
     * A distinct capability from the atomic notification above: a deferred
     * OPERATION. A post observer schedules a $delay() continuation that
     * runs on a later tick, writing a delivery-receipt marker on the room.
     * The receipt is NOT set the instant post_message returns (the work is
     * deferred); deferred_verify (call_out at t+4) confirms it appears once
     * the continuation has run. This exercises the substrate's
     * compiled-observer continuation -- a $delay inside a dispatcher-fired
     * observer -- and is kept distinct from phase 5's atomic notification
     * precisely because the two must not be conflated. */
    catch {
	deferred_room = spawn_room("ChatApp:Room:DeferredP8");
	CHAT_DAEMON->join_room(alice, deferred_room);

	MERRY_DAEMON->register_observer(deferred_room, "chat-room.message", "main",
					SRC_APPEND);
	MERRY_DAEMON->register_observer(deferred_room, "chat-room.message", "post",
					SRC_DEFERRED);

	CHAT_DAEMON->post_message(alice, deferred_room, "deferred please");

	if (deferred_room->query_raw_property("chat-room.delivery-receipt")) {
	    log_line("ChatApp:test: FAIL: DEFERRED-OP receipt landed synchronously (not deferred)");
	    return;
	}
    } : {
	log_line("ChatApp:test: FAIL: DEFERRED-OP setup threw");
	return;
    }

    /* phase 9: COHERENCE-SERIALIZE -- write coherence (concurrent-mutation
     * serialization), the platform's load-bearing coherent-multi-agent
     * guarantee.
     *
     * frank and grace both contend for the single open slot in a
     * capacity-1 room. Each claim_slot re-reads the room's CURRENT member
     * list at write time, and each call is its own atomic DGD task, so the
     * two claims serialize: the first task commits and fills the room; the
     * second task observes the post-commit membership and is refused.
     * Exactly one wins; the loser sees the updated (full) state and yields.
     * No lock, no coordination -- the runtime's coherent-state read is the
     * serialization. */
    {
	object room_c, frank, grace;
	int frank_won, grace_won;

	catch {
	    room_c = spawn_room("ChatApp:Room:CoherenceC");
	    frank  = spawn_user("frank");
	    grace  = spawn_user("grace");
	    room_c->set_capacity(1);

	    frank_won = room_c->claim_slot(frank);
	    grace_won = room_c->claim_slot(grace);
	} : {
	    log_line("ChatApp:test: FAIL: COHERENCE-SERIALIZE setup threw");
	    return;
	}
	if (frank_won + grace_won != 1) {
	    log_line("ChatApp:test: FAIL: COHERENCE-SERIALIZE not exactly one winner ("
		     + (string) frank_won + "/" + (string) grace_won + ")");
	    return;
	}
	if (sizeof(room_c->query_members()) != 1) {
	    log_line("ChatApp:test: FAIL: COHERENCE-SERIALIZE room not at capacity 1");
	    return;
	}
	/* the loser yielded: it is not a member and its claim returned 0 */
	if (frank_won && member(grace, room_c->query_members())) {
	    log_line("ChatApp:test: FAIL: COHERENCE-SERIALIZE loser grace still joined");
	    return;
	}
	if (grace_won && member(frank, room_c->query_members())) {
	    log_line("ChatApp:test: FAIL: COHERENCE-SERIALIZE loser frank still joined");
	    return;
	}
	log_line("ChatApp:test: COHERENCE-SERIALIZE OK");
    }

    /* phase 9b: LOST-UPDATE (negative) -- the failure mode the runtime's
     * coherent-state read removes by construction.
     *
     * Both writers compute their new member list from the SAME stale
     * snapshot, captured before either committed. The second write
     * (ivan's) overwrites the first (heidi's), so heidi's claim is lost
     * and only ivan remains. claim_slot above avoids this by reading
     * current state at write time; claim_slot_stale reproduces the lost
     * update by writing from the cached snapshot. This is the bug an
     * external coordination protocol would otherwise have to prevent. */
    {
	object room_l, heidi, ivan;
	object *stale;

	catch {
	    room_l = spawn_room("ChatApp:Room:LostUpdateL");
	    heidi  = spawn_user("heidi");
	    ivan   = spawn_user("ivan");

	    /* capture the member list ONCE, then both writers add themselves
	     * to that same stale snapshot */
	    stale = room_l->query_members();
	    room_l->claim_slot_stale(heidi, stale);
	    room_l->claim_slot_stale(ivan, stale);
	} : {
	    log_line("ChatApp:test: FAIL: LOST-UPDATE setup threw");
	    return;
	}
	if (member(heidi, room_l->query_members())) {
	    log_line("ChatApp:test: FAIL: LOST-UPDATE heidi survived (no lost update)");
	    return;
	}
	if (!member(ivan, room_l->query_members())) {
	    log_line("ChatApp:test: FAIL: LOST-UPDATE ivan missing");
	    return;
	}
	if (sizeof(room_l->query_members()) != 1) {
	    log_line("ChatApp:test: FAIL: LOST-UPDATE room not size 1 (lost update not reproduced)");
	    return;
	}
	log_line("ChatApp:test: LOST-UPDATE OK");
    }

    /* phase 9c: COHERENCE -- read coherence (all agents see the same state
     * at the same time), plus the cached-snapshot drift it removes.
     *
     * judy, kevin, and leo share one room with an append observer. Three
     * messages post in a known order. Each user reads the message log;
     * because the log is one runtime property -- not a per-user cached copy
     * -- all three reads return identical content in identical order. The
     * runtime carries the single coherent log; the readers reconcile
     * nothing.
     *
     * The CACHED-DIVERGE half is the read-side analogue of the phase-9b
     * lost update: judy caches the log, a fourth message posts, and her
     * cached copy now disagrees with the live shared log. The divergence is
     * the cost an application pays for caching state the runtime already
     * keeps coherent -- the failure mode the shared-state read removes. */
    {
	object room_r, judy, kevin, leo;
	mixed *seen_j, *seen_k, *seen_l;
	mixed *cached, *live;

	catch {
	    room_r = spawn_room("ChatApp:Room:ReadOrderR");
	    judy   = spawn_user("judy");
	    kevin  = spawn_user("kevin");
	    leo    = spawn_user("leo");
	    CHAT_DAEMON->join_room(judy, room_r);
	    CHAT_DAEMON->join_room(kevin, room_r);
	    CHAT_DAEMON->join_room(leo, room_r);

	    MERRY_DAEMON->register_observer(room_r, "chat-room.message", "main",
					    SRC_APPEND);

	    CHAT_DAEMON->post_message(judy, room_r, "one");
	    CHAT_DAEMON->post_message(kevin, room_r, "two");
	    CHAT_DAEMON->post_message(leo, room_r, "three");

	    /* three independent reads of the one shared log */
	    seen_j = room_r->query_messages();
	    seen_k = room_r->query_messages();
	    seen_l = room_r->query_messages();
	} : {
	    log_line("ChatApp:test: FAIL: COHERENCE setup threw");
	    return;
	}
	if (sizeof(seen_j) != 3 || sizeof(seen_k) != 3 || sizeof(seen_l) != 3) {
	    log_line("ChatApp:test: FAIL: COHERENCE reader saw wrong message count");
	    return;
	}
	if (seen_j[0]["content"] != "one" || seen_j[1]["content"] != "two" ||
	    seen_j[2]["content"] != "three") {
	    log_line("ChatApp:test: FAIL: COHERENCE message order not preserved");
	    return;
	}
	if (seen_j[0]["content"] != seen_k[0]["content"] ||
	    seen_k[0]["content"] != seen_l[0]["content"] ||
	    seen_j[2]["content"] != seen_k[2]["content"] ||
	    seen_k[2]["content"] != seen_l[2]["content"]) {
	    log_line("ChatApp:test: FAIL: COHERENCE readers disagree on order");
	    return;
	}
	log_line("ChatApp:test: COHERENCE OK");

	/* cached-snapshot drift: capture the log, post once more, compare */
	catch {
	    cached = room_r->query_messages();
	    CHAT_DAEMON->post_message(judy, room_r, "four");
	    live = room_r->query_messages();
	} : {
	    log_line("ChatApp:test: FAIL: CACHED-DIVERGE post threw");
	    return;
	}
	if (sizeof(cached) != 3) {
	    log_line("ChatApp:test: FAIL: CACHED-DIVERGE cached snapshot mutated");
	    return;
	}
	if (sizeof(live) != 4) {
	    log_line("ChatApp:test: FAIL: CACHED-DIVERGE live log did not advance");
	    return;
	}
	log_line("ChatApp:test: CACHED-DIVERGE OK");
    }

    /* phase 9d: ATOMIC-COMMIT / ATOMIC-ROLLBACK -- atomic cross-room writes
     * through the MERRY batch surface, composed with DGD atomicity.
     *
     * cross_write appends one message to TWO rooms; wrapped in
     * MERRY->batch(..., atomic: 1) the two appends are one atomic unit. The
     * commit path lands both. The deliberate-failure path
     * (cross_write_then_fail appends to the first room, then throws) rolls
     * BOTH writes back -- cross-room, where phase 7 showed single-room. The
     * runtime carries the all-or-nothing boundary across two separate room
     * objects with no application-level transaction code. */
    {
	object room_ba, room_bb;
	int threw;

	catch {
	    room_ba = spawn_room("ChatApp:Room:BatchA");
	    room_bb = spawn_room("ChatApp:Room:BatchB");

	    MERRY_DAEMON->batch(this_object(), "cross_write",
				({ room_ba, room_bb, "commit" }),
				([ "atomic": 1 ]));
	} : {
	    log_line("ChatApp:test: FAIL: ATOMIC-COMMIT batch threw");
	    return;
	}
	if (sizeof(room_ba->query_messages()) != 1 ||
	    sizeof(room_bb->query_messages()) != 1) {
	    log_line("ChatApp:test: FAIL: ATOMIC-COMMIT both rooms not written");
	    return;
	}
	log_line("ChatApp:test: ATOMIC-COMMIT OK");

	threw = 0;
	catch {
	    MERRY_DAEMON->batch(this_object(), "cross_write_then_fail",
				({ room_ba, room_bb, "rollback" }),
				([ "atomic": 1 ]));
	} : {
	    threw = 1;
	}
	if (!threw) {
	    log_line("ChatApp:test: FAIL: ATOMIC-ROLLBACK batch did not throw");
	    return;
	}
	/* both rooms still hold only the committed message; the failed
	 * batch's partial write to room_ba rolled back with the throw. */
	if (sizeof(room_ba->query_messages()) != 1 ||
	    sizeof(room_bb->query_messages()) != 1) {
	    log_line("ChatApp:test: FAIL: ATOMIC-ROLLBACK partial write survived");
	    return;
	}
	log_line("ChatApp:test: ATOMIC-ROLLBACK OK");
    }

    /* phase 9d (negative): PARTIAL-STATE -- the same cross-room writer run
     * through a NON-atomic batch. Without the atomic envelope, the first
     * room's append commits before the throw, so it survives while the
     * second room is never written: partial state on error. This is what
     * the atomic batch above removes -- the contrast that shows why the
     * all-or-nothing boundary matters. Fresh rooms so the partial write is
     * unambiguous (one ends with a message, the other empty). */
    {
	object room_pa, room_pb;
	int threw;

	threw = 0;
	catch {
	    room_pa = spawn_room("ChatApp:Room:PartialA");
	    room_pb = spawn_room("ChatApp:Room:PartialB");

	    /* no "atomic" opt: a plain batch run, throw is not rolled back */
	    MERRY_DAEMON->batch(this_object(), "cross_write_then_fail",
				({ room_pa, room_pb, "partial" }),
				([ ]));
	} : {
	    threw = 1;
	}
	if (!threw) {
	    log_line("ChatApp:test: FAIL: PARTIAL-STATE non-atomic batch did not throw");
	    return;
	}
	/* the first write survived (no rollback); the second never ran */
	if (sizeof(room_pa->query_messages()) != 1) {
	    log_line("ChatApp:test: FAIL: PARTIAL-STATE first write did not survive");
	    return;
	}
	if (sizeof(room_pb->query_messages()) != 0) {
	    log_line("ChatApp:test: FAIL: PARTIAL-STATE second room unexpectedly written");
	    return;
	}
	log_line("ChatApp:test: PARTIAL-STATE OK");
    }

    /* phase 9e: ANCESTRY / NO-INHERIT -- one observer registered on a room
     * ancestor fans out to every room in the cohort via the UrHierarchy
     * walk.
     *
     * A base room carries the append observer; three child rooms set the
     * base as their ur-object but register NOTHING themselves. A message
     * posting in each child resolves the base's observer through the
     * ancestry walk -- $this is the child, so the append lands on the
     * child's own log. The coherent reactive behavior is provided once, at
     * the ancestor, to every cohort member: no per-room registration. This
     * re-exercises the dispatcher ancestry walk at the application
     * tier (cf. the merry-app DISPATCH ANCESTRY phase).
     *
     * The NO-INHERIT half is the contrast: a room with neither its own
     * observer NOR an ancestor carrying one. The message posts, the
     * ancestry walk finds no append observer at any level, and nothing
     * records it -- the log stays empty. This is what every room would
     * require absent the inherited observer: its own registration. */
    {
	object base, child_x, child_y, child_z, niaj;
	object lone, olivia;

	catch {
	    base    = spawn_room("ChatApp:Room:AncestryBase");
	    child_x = spawn_room("ChatApp:Room:AncestryX");
	    child_y = spawn_room("ChatApp:Room:AncestryY");
	    child_z = spawn_room("ChatApp:Room:AncestryZ");
	    niaj    = spawn_user("niaj");

	    /* register the append observer ONCE, on the ancestor */
	    MERRY_DAEMON->register_observer(base, "chat-room.message", "main",
					    SRC_APPEND);

	    child_x->set_ur_object(base);
	    child_y->set_ur_object(base);
	    child_z->set_ur_object(base);

	    CHAT_DAEMON->post_message(niaj, child_x, "x");
	    CHAT_DAEMON->post_message(niaj, child_y, "y");
	    CHAT_DAEMON->post_message(niaj, child_z, "z");
	} : {
	    log_line("ChatApp:test: FAIL: ANCESTRY setup threw");
	    return;
	}
	if (sizeof(child_x->query_messages()) != 1 ||
	    sizeof(child_y->query_messages()) != 1 ||
	    sizeof(child_z->query_messages()) != 1) {
	    log_line("ChatApp:test: FAIL: ANCESTRY inherited observer did not fire on all clones");
	    return;
	}
	log_line("ChatApp:test: ANCESTRY OK");

	catch {
	    lone   = spawn_room("ChatApp:Room:NoInheritL");
	    olivia = spawn_user("olivia");
	    /* no ur-object, no observer registered */
	    CHAT_DAEMON->post_message(olivia, lone, "unheard");
	} : {
	    log_line("ChatApp:test: FAIL: NO-INHERIT setup threw");
	    return;
	}
	if (sizeof(lone->query_messages()) != 0) {
	    log_line("ChatApp:test: FAIL: NO-INHERIT message recorded without an observer");
	    return;
	}
	log_line("ChatApp:test: NO-INHERIT OK");
    }

    /* phase 10: PERSIST SETUP
     *
     * Establish a chat session on a fresh room: alice and bob join,
     * exchange three messages, and alice's mention-tracker is set to a
     * cross-clone reference to bob's user object. Save the room and the
     * two accounts as object globals so the post-restore phase 11 can
     * resolve them, record the session shape to an on-disk marker (the
     * negative-case path reads this back on a cold boot), schedule the
     * verify call_out, then dump a snapshot and exit via the System
     * helper.
     *
     * dump_state(FALSE) captures the room clone, the two user clones,
     * the three message mappings, the cross-clone mention-tracker
     * reference, and the scheduled persist_verify call_out. The
     * external smoke harness restarts DGD against the snapshot; the
     * surviving call_out fires phase 11 as soon as the system is back
     * up (its scheduled time has elapsed during the restart).
     */
    catch {
	mixed *msgs;
	object *members;

	persist_room  = spawn_room("ChatApp:Room:PersistD");
	persist_alice = alice;
	persist_bob   = bob;

	/* post_message routes the append through an observer, so the
	 * persist room needs the append-to-history observer registered
	 * before its messages post. No mention-notify observer here -- the
	 * persistence session does not exercise async notification, and the
	 * "@bob ping" message stays a plain logged message. */
	MERRY_DAEMON->register_observer(persist_room, "chat-room.message", "main",
					SRC_APPEND);

	CHAT_DAEMON->join_room(alice, persist_room);
	CHAT_DAEMON->join_room(bob, persist_room);
	CHAT_DAEMON->post_message(alice, persist_room, "hi bob");
	CHAT_DAEMON->post_message(bob, persist_room, "hey alice");
	CHAT_DAEMON->post_message(alice, persist_room, "@bob ping");

	/* a cross-clone semantic reference: alice's mention-tracker
	 * points AT bob's user object. DGD orthogonal persistence must
	 * resurrect this as the same bob clone (an object reference),
	 * not a copy, after restore. */
	alice->set_property("chat-user.mention-tracker", ({ bob }));

	msgs    = persist_room->query_messages();
	members = persist_room->query_members();
	if (sizeof(msgs) != 3 || sizeof(members) != 2) {
	    log_line("ChatApp:test: FAIL: PERSIST-SETUP session shape ("
		     + (string) sizeof(msgs) + " msgs, "
		     + (string) sizeof(members) + " members)");
	    return;
	}

	/* on-disk marker recording the pre-dump session shape. Files
	 * survive any boot (they are not in-image state); the cold-boot
	 * negative path reads this back to prove the session existed
	 * before it was lost. */
	catch(remove_file(MARKER_FILE));
	write_file(MARKER_FILE, "messages=3 members=2\n");

	log_line("ChatApp:test: PERSIST-SETUP OK");
    } : {
	log_line("ChatApp:test: FAIL: PERSIST-SETUP threw");
	return;
    }

    /* Boot-1 finalization. Three call_outs, ordered so phase 8's deferred
     * operation lands on this boot and the persistence assertion lands on
     * the next:
     *
     *   t+4  deferred_verify -- phase 8's $delay continuation (t+2) has run
     *                           and written the delivery receipt; fires
     *                           before the dump.
     *   t+5  finalize_dump   -- trigger_dump_and_exit takes the snapshot.
     *   t+8  persist_verify  -- scheduled but NOT yet fired at dump time,
     *                           so DGD captures it pending; it fires as
     *                           soon as the external restore is back up.
     *
     * persist_verify's delay must exceed the dump time so it survives into
     * the snapshot rather than firing on boot 1. */
    call_out("deferred_verify", 4);
    call_out("finalize_dump", 5);
    call_out("persist_verify", 8);
}


/* phase 7 helper: post a message inside a DGD atomic envelope, then throw.
 * The atomic modifier wraps the whole body; the error rolls back every
 * write the task made -- the message-log append and the synchronous
 * mention-notify observer's cross-user Set alike. The caller catches the
 * throw and asserts neither write survived. */
static atomic void atomic_post_then_fail(object sender, object room,
					 string content)
{
    CHAT_DAEMON->post_message(sender, room, content);
    error("EVENT-ROLLBACK deliberate failure");
}


/* phase 9d helpers: append one message to TWO rooms. cross_write is the
 * commit path; cross_write_then_fail appends to the first room then throws,
 * so the MERRY batch's atomic mode must roll back the partial first write
 * (the second is never reached). Public and non-static so MERRY's batch()
 * can reach them via call_other (mirrors the merry-app _throw_for_test
 * helper). The append is a direct message-log write, not a dispatched post,
 * so the demonstration isolates the atomic batch boundary rather than the
 * observer fan-out. */
void cross_write(object room_a, object room_b, string content)
{
    mapping msg;

    msg = ([ "content": content, "timestamp": time() ]);
    room_a->set_property("chat-room.message-log",
			 room_a->query_messages() + ({ msg }));
    room_b->set_property("chat-room.message-log",
			 room_b->query_messages() + ({ msg }));
}

void cross_write_then_fail(object room_a, object room_b, string content)
{
    mapping msg;

    msg = ([ "content": content, "timestamp": time() ]);
    room_a->set_property("chat-room.message-log",
			 room_a->query_messages() + ({ msg }));
    error("ATOMIC-ROLLBACK deliberate failure");
}


/* phase 8 verification, firing after the $delay continuation (t+2) but
 * before the snapshot dump (t+5). Confirms the deferred operation's
 * delivery receipt appeared on the later tick. */
static void deferred_verify()
{
    catch {
	if (!deferred_room->query_raw_property("chat-room.delivery-receipt")) {
	    log_line("ChatApp:test: FAIL: DEFERRED-OP receipt did not land after delay");
	    return;
	}
	log_line("ChatApp:test: DEFERRED-OP OK");
    } : {
	log_line("ChatApp:test: FAIL: deferred_verify threw");
    }
}


/* Snapshot trigger, deferred past deferred_verify so the boot-1
 * assertions complete before the dump. Routes through the System persist
 * helper's call_out-deferred dump_state(FALSE) + shutdown(). */
static void finalize_dump()
{
    PERSIST_HELPER->trigger_dump_and_exit();
}


/* phase 11: PERSIST VERIFY -- fires from the pre-snapshot call_out
 * after the restore. Confirms the chat session survived the snapshot
 * cycle: three messages, two member accounts (resolved as live clones
 * with their names intact), the cross-clone mention-tracker reference
 * (the same bob object, not a copy), and that a third user (carol)
 * joining the restored room observes all prior state. */
static void persist_verify()
{
    object carol;
    mixed *msgs, *mention;
    object *members;

    if (!persist_room) {
	log_line("ChatApp:test: FAIL: PERSIST-VERIFY persist_room nil after restore");
	return;
    }

    catch {
	members = persist_room->query_members();
	msgs    = persist_room->query_messages();

	if (sizeof(msgs) != 3) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY message count "
		     + (string) sizeof(msgs) + " after restore");
	    return;
	}
	if (sizeof(members) != 2) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY member count "
		     + (string) sizeof(members) + " after restore");
	    return;
	}

	/* user accounts survived as live clones with their names. */
	if (persist_alice->query_chat_name() != "alice" ||
	    persist_bob->query_chat_name() != "bob") {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY user account names lost");
	    return;
	}

	/* the cross-clone mention-tracker reference resurrected as the
	 * same bob object, not a copy. */
	mention = persist_alice->query_mention_tracker();
	if (sizeof(mention) != 1 || mention[0] != persist_bob) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY mention-tracker cross-clone ref lost");
	    return;
	}

	/* a third user joining the restored room observes the full prior
	 * session: the three messages and the now-three-member roster. */
	carol = spawn_user("carol-restored");
	CHAT_DAEMON->join_room(carol, persist_room);
	if (!member(carol, persist_room->query_members())) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY carol join failed");
	    return;
	}
	if (sizeof(persist_room->query_messages()) != 3) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY carol sees wrong message count");
	    return;
	}
	if (sizeof(persist_room->query_members()) != 3) {
	    log_line("ChatApp:test: FAIL: PERSIST-VERIFY roster not 3 after carol join");
	    return;
	}
    } : {
	log_line("ChatApp:test: FAIL: PERSIST-VERIFY threw");
	return;
    }

    log_line("ChatApp:test: PERSIST-VERIFY OK");
}


private object spawn_user(string name)
{
    object u;

    u = clone_object(USER_PROG);
    u->set_object_name("ChatApp:User:" + name);
    u->set_chat_name(name);
    return u;
}


private object spawn_room(string id)
{
    object r;

    r = clone_object(ROOM_PROG);
    r->set_object_name(id);
    r->set_id(id);
    return r;
}


private int marker_present()
{
    return file_info(MARKER_FILE) ? 1 : 0;
}


/* Negative-case verification. Reached from setup_and_run on a cold boot
 * (no snapshot loaded) that follows the phase-10 boot. The marker file
 * (on disk) survived the cold boot; the in-image chat session
 * (persist_room and its clones) did not. This demonstrates that
 * in-memory-only state does not survive a cold boot without a snapshot,
 * while on-disk records persist independently of the runtime image. */
private void coldboot_loss_verify()
{
    string marker;

    marker = nil;
    catch { marker = read_file(MARKER_FILE); }
    if (!marker) {
	log_line("ChatApp:test: FAIL: COLDBOOT-LOST marker unreadable");
	return;
    }
    /* The marker proves the session existed before this boot. If the
     * in-image room were also present, the snapshot would have been
     * loaded and this path would not have been taken -- so a non-nil
     * persist_room here is a contradiction. */
    if (persist_room) {
	log_line("ChatApp:test: FAIL: COLDBOOT-LOST room unexpectedly present");
	return;
    }
    log_line("ChatApp:test: COLDBOOT-LOST OK");
}


private void log_line(string msg)
{
    mixed *info;
    int size;

    catch {
	info = file_info(RESULT_FILE);
	size = info ? info[0] : 0;
	write_file(RESULT_FILE, msg + "\n", size);
    }
}
