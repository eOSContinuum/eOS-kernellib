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
 *   1.  CAP-REJECT OK     -- regular user calls admin->kick without
 *                            an admin-token; capability check throws.
 *   2.  CAP-ACCEPT OK     -- admin user holding a per-room kick
 *                            token calls admin->kick; target leaves
 *                            the room.
 *   3.  SANDBOX-ACCEPT OK -- a per-room Merry reaction whose source
 *                            calls only in-sandbox kfuns compiles,
 *                            binds on the room under
 *                            merry:on:chat-room.message:main, fires
 *                            via run_merry after a message post, and
 *                            the marker property it sets lands.
 *   4.  SANDBOX-REJECT OK -- a sibling reaction whose source calls a
 *                            sandbox-denied kfun (write_file) compiles
 *                            (registration succeeds) but errors on its
 *                            first fire with the merrynode SANDBOX
 *                            shadow's "function not allowed in merry
 *                            code"; no file is created.
 *  10.  PERSIST-SETUP OK  -- establish a chat session (2 users, 3
 *                            messages, a cross-clone mention-tracker
 *                            reference) on a fresh room, record the
 *                            session shape to an on-disk marker, then
 *                            dump a snapshot and exit.
 *  11.  PERSIST-VERIFY OK -- fires from a surviving call_out after the
 *                            snapshot is restored; the whole session
 *                            survived and a third user joining the
 *                            restored room observes all prior state.
 *      COLDBOOT-LOST OK   -- negative case. On a cold boot taken
 *                            WITHOUT loading the snapshot, the on-disk
 *                            marker survived but the in-memory session
 *                            did not; in-memory-only state does not
 *                            survive a cold boot without a snapshot.
 *
 * The persistence phases follow the two-boot pattern of
 * examples/merry-app/sys/test.c phases 16/17, plus a third (cold,
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
 * methods resolve at compile time -- the SkotOS convention merryapi.c
 * was authored against, mirrored by examples/merry-app/sys/test.c. The
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
# define HACK_FILE      "/usr/Chat/data/hack.txt"

static void run_tests();
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

    /* phase 3: SANDBOX-ACCEPT
     *
     * Load a per-room reaction as Merry code. The source calls only
     * the in-sandbox Set() merryfun -- no kfun in merrynode.c's
     * SANDBOX() deny list -- so it both compiles and fires cleanly.
     * The script is bound on room_a under the reaction-property key
     * merry:on:chat-room.message:main (the merry:on:<path>:<timing>
     * convention the property-change dispatcher reads); here it is
     * fired directly via run_merry to demonstrate sandboxed code load
     * in isolation. Wiring the same reaction to fire automatically off
     * the dispatcher's post-timing observers on a message post is the
     * async-event phase's scope; this phase proves only that loaded
     * Merry code executes within the sandbox surface and mutates room
     * state.
     *
     * bob posts a message to room_a (he is still a member after the
     * phase-2 kick removed alice). The reaction is fired with the new
     * message count as the $count argument; it sets the room's
     * chat-room.message-count marker property to that value. The
     * assertion reads the marker back.
     */
    catch {
	object accept_script;
	int count;

	accept_script = new_object(MERRY_DATA,
		"Set($this, \"chat-room.message-count\", $count);");
	room_a->set_property("merry:on:chat-room.message:main",
			     accept_script);

	CHAT_DAEMON->post_message(bob, room_a, "first post");
	count = sizeof(room_a->query_messages());

	run_merry(room_a, "chat-room.message:main", "on",
		  ([ "count": count ]));

	if (room_a->query_raw_property("chat-room.message-count") != count) {
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

	/* schedule the verify with a small delay so the call_out is in
	 * the snapshot. After restore its scheduled time has long passed
	 * and DGD fires it as soon as the system is back up. */
	call_out("persist_verify", 3);

	log_line("ChatApp:test: PERSIST-SETUP OK");

	PERSIST_HELPER->trigger_dump_and_exit();
    } : {
	log_line("ChatApp:test: FAIL: PERSIST-SETUP threw");
	return;
    }
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
