/*
 * Boot-time test driver for the Chat application example.
 *
 * Exercises the FX-2 primitives via 11 phases mapped across
 * PD-1..PD-5. Each phase writes a sentinel line to the result file
 * at /usr/Chat/data/test-result.log on success; failures write a
 * distinct FAIL sentinel. Phases are wrapped in catch{} so a failure
 * in one does not mask a different failure in another.
 *
 * PD-1 scope: phases 1 + 2 (capability separation via admin-token).
 * Phases 3-11 are wired in as later PD-N tasks close (PD-2..PD-5).
 *
 *   1. PD1-CAP-REJECT OK  -- regular user calls admin->kick without
 *                            an admin-token; capability check throws.
 *   2. PD1-CAP-ACCEPT OK  -- admin user holding a per-room kick
 *                            token calls admin->kick; target leaves
 *                            the room.
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

# define CHAT_DAEMON  "/usr/Chat/sys/chat"
# define ADMIN_DAEMON "/usr/Chat/sys/admin"
# define ROOM_PROG    "/usr/Chat/obj/room"
# define USER_PROG    "/usr/Chat/obj/user"
# define RESULT_FILE  "/usr/Chat/data/test-result.log"

static void run_tests();
private void log_line(string msg);
private object spawn_user(string name);
private object spawn_room(string id);


static void create()
{
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    catch(make_dir("/usr/Chat/data"));
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

    /* phase 1: PD1-CAP-REJECT
     *
     * bob (no admin token) calls admin->kick. The capability check
     * walks bob's admin-tokens list, finds nothing matching room_a +
     * "kick", and throws. The catch{} below converts the throw into
     * a PD1-CAP-REJECT OK sentinel.
     */
    rejected = 0;
    catch {
	ADMIN_DAEMON->kick(bob, alice, room_a);
    } : {
	rejected = 1;
    }
    if (!rejected) {
	log_line("ChatApp:test: FAIL: PD1-CAP-REJECT kick did not throw");
	return;
    }
    /* The reject must not have removed alice from the room. */
    if (!member(alice, room_a->query_members())) {
	log_line("ChatApp:test: FAIL: PD1-CAP-REJECT membership mutated");
	return;
    }
    log_line("ChatApp:test: PD1-CAP-REJECT OK");

    /* phase 2: PD1-CAP-ACCEPT
     *
     * carol receives a per-room "kick" admin token. carol calls
     * admin->kick; the capability check matches and the verb removes
     * alice from room_a. The catch{} below records FAIL if anything
     * throws.
     */
    catch {
	token = ADMIN_DAEMON->grant_admin(nil, carol, room_a, ({ "kick" }));
    } : {
	log_line("ChatApp:test: FAIL: PD1-CAP-ACCEPT grant_admin threw");
	return;
    }
    if (!token) {
	log_line("ChatApp:test: FAIL: PD1-CAP-ACCEPT grant_admin returned nil");
	return;
    }

    catch {
	ADMIN_DAEMON->kick(carol, alice, room_a);
    } : {
	log_line("ChatApp:test: FAIL: PD1-CAP-ACCEPT kick threw");
	return;
    }
    if (member(alice, room_a->query_members())) {
	log_line("ChatApp:test: FAIL: PD1-CAP-ACCEPT alice still member");
	return;
    }
    if (member(room_a, alice->query_subscriptions())) {
	log_line("ChatApp:test: FAIL: PD1-CAP-ACCEPT subscription persisted");
	return;
    }
    log_line("ChatApp:test: PD1-CAP-ACCEPT OK");

    /* phase 3-11: filled in by PD-2..PD-5 task closures. */
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
