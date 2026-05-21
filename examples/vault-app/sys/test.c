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
# define RESULT_FILE	"/usr/MyApp/data/test-result.log"

static void run_tests();
private void register_thing_schema();
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
