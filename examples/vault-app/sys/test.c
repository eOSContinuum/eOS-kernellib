/*
 * Boot-time test driver for the Vault application example.
 *
 * Inherits lib/app to register as a Vault node, registers a MyApp:Thing
 * schema_node so the demonstration clonable has a marshaling shape, and
 * runs a round-trip assertion via a delayed call_out so the assertion
 * fires after every domain's initd has returned (and the Schema +
 * Vault daemons are both up).
 *
 * Assertion path:
 *   1. clone a thing, set label + count, set_object_name "MyApp:demo:thing1"
 *   2. Vault->store(thing) -- emits XML to .runtime/state/vault/data/MyApp/demo/thing1.xml
 *      via export_state -> Schema callbacks (query_label / query_count)
 *   3. destruct the original clone
 *   4. Vault->spawn_one_by_name("MyApp:demo:thing1") -- reads the XML, clones a
 *      fresh thing, dispatches set_label / set_count via import_state
 *   5. resolve the reloaded clone via Index (find_named, lifted at LV-4.5d)
 *   6. assert query_label and query_count match
 *
 * A second assertion set (run_singleton_test) exercises the singleton
 * <object> storage shape with sys/config (a one-of-a-kind daemon):
 * store + re-import through the public Vault API, the cross-domain
 * compile boundary (the Vault daemon cannot compile another domain's
 * unloaded program), and the supported owning-domain respawn through
 * the driver's inherited vault_node spawn functions.
 *
 * Pass/fail is observable two ways:
 *
 *   - A sentinel file at /usr/MyApp/data/test-result.log carries the
 *     final status line. The verify command in README.md cats it.
 *   - Failures throw via error() inside the call_out frame, which DGD
 *     surfaces as "[caught]" entries in boot.log with a stack trace.
 *
 * DRIVER->message() would surface success directly in boot.log but
 * requires KERNEL() or SYSTEM() previous_program; MyApp is neither.
 * The sentinel-file path is what an application-tier daemon can do
 * without a kernel-layer log facility (the lifted sysLog is currently
 * a no-op stub per LV-4.5b Decision pending such a facility).
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/MyApp/lib/app";
inherit "/lib/util/named";

# define VAULT		"/usr/Vault/sys/vault"
# define SCHEMA		"/usr/Schema/sys/schema_daemon"
# define SCHEMA_NODE	"/usr/Schema/obj/schema_node"
# define INDEX		"/usr/Index/sys/index_daemon"

# define ROOT		"/usr/MyApp/data/things"
# define THING_PROG	"/usr/MyApp/obj/thing"
# define DEMO_NAME	"MyApp:demo:thing1"
# define CONFIG_PROG	"/usr/MyApp/sys/config"
# define CONFIG_NAME	"MyApp:config:main"
/* where Vault->store lands CONFIG_NAME on disk: the Vault root plus the
 * colon-separated name with colons as directory separators. */
# define VAULT_FILE	"/usr/Vault/data/vault/MyApp/config/main.xml"
# define RESULT_FILE	"/usr/MyApp/data/test-result.log"

static void run_tests();
static void run_singleton_test();
private void register_thing_schema();
private void register_config_schema();
private void log_line(string msg);


static void create()
{
    /* Defer everything to a call_out so the Vault daemon and the
     * Schema daemon are both loaded by the time we register. The
     * domain-load order in /usr/System/initd is alphabetical, so
     * MyApp's initd fires before Vault's (and before Schema's),
     * which means ::create(ROOT) -> vault_node::create -> VAULT->
     * register_node would call into a not-yet-loaded daemon. The
     * call_out fires after System/initd::create() returns, by which
     * point all per-domain initds have run and the daemons exist.
     */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    ::create(ROOT);
    set_object_name("MyApp:TestDriver");
    register_thing_schema();
    register_config_schema();
    /* /usr/MyApp/data/ may not exist on first boot; create before write. */
    catch(make_dir("/usr/MyApp/data"));
    /* Clear any prior boot's sentinel; a fresh run rewrites from byte 0. */
    catch(remove_file(RESULT_FILE));
    run_tests();
}


private void register_thing_schema()
{
    object node;

    node = clone_object(SCHEMA_NODE);
    node->set_name("MyApp", "Thing");
    node->add_attribute("label", "lpc_str", "query_label");
    node->add_attribute("count", "lpc_int", "query_count");
    node->add_callback("set_label", "label");
    node->add_callback("set_count", "count");
}


private void register_config_schema()
{
    object node;

    node = clone_object(SCHEMA_NODE);
    node->set_name("MyApp", "Config");
    node->add_attribute("greeting", "lpc_str", "query_greeting");
    node->add_attribute("limit", "lpc_int", "query_limit");
    node->add_callback("set_greeting", "greeting");
    node->add_callback("set_limit", "limit");
}


static void run_tests()
{
    object thing, reloaded, indexed;
    string label;
    int count;

    log_line("MyApp:test: starting round-trip");

    /* phase 1: create, set, store */
    thing = clone_object(THING_PROG);
    thing->set_object_name(DEMO_NAME);
    thing->set_label("hello");
    thing->set_count(42);

    catch {
	VAULT->store(thing);
    } : {
	log_line("MyApp:test: FAIL: store threw");
	return;
    }

    destruct_object(thing);

    /* phase 2: reload from disk */
    catch {
	VAULT->spawn_one_by_name(DEMO_NAME);
    } : {
	log_line("MyApp:test: FAIL: spawn_one_by_name threw");
	return;
    }

    reloaded = find_named(DEMO_NAME);
    if (!reloaded) {
	log_line("MyApp:test: FAIL: reloaded object not findable by name");
	return;
    }

    label = reloaded->query_label();
    count = reloaded->query_count();

    if (label != "hello") {
	log_line("MyApp:test: FAIL: label round-trip "
		 + (label ? "\"" + label + "\"" : "(nil)")
		 + " != \"hello\"");
	return;
    }
    if (count != 42) {
	log_line("MyApp:test: FAIL: count round-trip "
		 + (string) count + " != 42");
	return;
    }

    /* phase 3: Index lookup matches Vault-restored object identity */
    indexed = INDEX->query_object(DEMO_NAME);
    if (indexed != reloaded) {
	log_line("MyApp:test: FAIL: Index lookup did not match reloaded");
	return;
    }

    log_line("MyApp:test: ROUND-TRIP OK");

    run_singleton_test();
}


static void run_singleton_test()
{
    object config, respawned;
    string text;

    /* phase 4: singleton <object> storage path. The round-trip above
     * exercises the <clone> root element (clone_object on respawn);
     * a one-of-a-kind daemon stores as <object program="..."> instead.
     * Three assertions cover the singleton surface:
     *
     *   4a  store + re-import through the public Vault API against a
     *       LOADED singleton: mutate the live state after the store,
     *       respawn by name, assert the stored values win.
     *   4b  the cross-domain boundary: after a destruct, the Vault
     *       daemon CANNOT compile another domain's program fresh --
     *       kernel compile_object grants non-lib compiles only with
     *       write access to the path, and the Vault has none over
     *       /usr/MyApp. do_spawn catches the access error internally
     *       (a "[caught]" trace in the boot log is expected), so the
     *       assertion is that the program stays unloaded.
     *   4c  the supported respawn of an unloaded singleton: the
     *       OWNING domain's vault node (this driver) calls its
     *       inherited spawn_create_one / spawn_configure_one, which
     *       compile the program in MyApp's own context.
     */

    /* 4a: store, mutate, respawn-by-name, assert stored state wins. */
    config = compile_object(CONFIG_PROG);
    config->set_object_name(CONFIG_NAME);
    config->set_greeting("stored");
    config->set_limit(7);

    catch {
	VAULT->store(config);
    } : {
	log_line("MyApp:test: FAIL: singleton store threw");
	return;
    }

    config->set_greeting("mutated");
    config->set_limit(99);

    catch {
	VAULT->spawn_one_by_name(CONFIG_NAME);
    } : {
	log_line("MyApp:test: FAIL: singleton respawn threw");
	return;
    }

    if (config->query_greeting() != "stored" || config->query_limit() != 7) {
	log_line("MyApp:test: FAIL: singleton re-import did not restore "
		 + "stored state (greeting "
		 + (config->query_greeting() ?
		    "\"" + config->query_greeting() + "\"" : "(nil)")
		 + ", limit " + (string) config->query_limit() + ")");
	return;
    }

    log_line("MyApp:test: SINGLETON OK");

    /* 4b: cross-domain compile boundary. With the program unloaded,
     * the Vault daemon's find-or-load cannot compile /usr/MyApp/sys/
     * config (kernel write-access rule); the error is caught inside
     * do_spawn, so spawn_one_by_name returns normally and the program
     * must still be unloaded afterwards. */
    destruct_object(config);

    catch {
	VAULT->spawn_one_by_name(CONFIG_NAME);
    } : {
	log_line("MyApp:test: FAIL: boundary respawn threw to caller");
	return;
    }

    if (find_object(CONFIG_PROG)) {
	log_line("MyApp:test: FAIL: cross-domain respawn unexpectedly "
		 + "loaded the program");
	return;
    }

    log_line("MyApp:test: XDOMAIN-RESPAWN-REJECT OK");

    /* 4c: owning-domain respawn. This driver is itself a vault node
     * (lib/app inherits ~Vault/lib/vault_node), so it can respawn its
     * domain's unloaded singleton from the stored XML: the inherited
     * spawn_create_one compiles the program in MyApp's own context,
     * and spawn_configure_one re-imports the stored state. */
    text = read_file(VAULT_FILE);
    if (!text) {
	log_line("MyApp:test: FAIL: stored singleton XML not readable");
	return;
    }

    catch {
	spawn_create_one(CONFIG_NAME, VAULT_FILE, text);
	spawn_configure_one(CONFIG_NAME, VAULT_FILE, text);
    } : {
	log_line("MyApp:test: FAIL: owning-domain respawn threw");
	return;
    }

    respawned = find_object(CONFIG_PROG);
    if (!respawned) {
	log_line("MyApp:test: FAIL: owning-domain respawn did not load "
		 + "the program");
	return;
    }
    if (find_named(CONFIG_NAME) != respawned) {
	log_line("MyApp:test: FAIL: respawned singleton name lookup "
		 + "mismatch");
	return;
    }
    if (respawned->query_greeting() != "stored" ||
	respawned->query_limit() != 7) {
	log_line("MyApp:test: FAIL: respawned singleton state mismatch "
		 + "(greeting "
		 + (respawned->query_greeting() ?
		    "\"" + respawned->query_greeting() + "\"" : "(nil)")
		 + ", limit " + (string) respawned->query_limit() + ")");
	return;
    }

    log_line("MyApp:test: NODE-RESPAWN OK");
}


private void log_line(string msg)
{
    /* Append the line to the sentinel result file. The MyApp domain
     * owns /usr/MyApp/data/ so write_file works without an access
     * grant. setup_and_run guarantees the parent directory exists.
     * The verify command in README.md cats this file. */
    mixed *info;
    int size;

    catch {
	info = file_info(RESULT_FILE);
	size = info ? info[0] : 0;
	write_file(RESULT_FILE, msg + "\n", size);
    }
}
