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
 *   2. Vault->store(thing) -- emits XML to /usr/Vault/data/vault/MyApp/demo/thing1.xml
 *      via export_state -> Schema callbacks (query_label / query_count)
 *   3. destruct the original clone
 *   4. Vault->spawn_one_by_name("MyApp:demo:thing1") -- reads the XML, clones a
 *      fresh thing, dispatches set_label / set_count via import_state
 *   5. resolve the reloaded clone via Index (find_named)
 *   6. assert query_label and query_count match
 *
 * A second assertion set (run_singleton_test) exercises the singleton
 * <object> storage shape with sys/config (a one-of-a-kind daemon):
 * store + re-import through the public Vault API, the cross-domain
 * compile boundary (the Vault daemon cannot compile another domain's
 * unloaded program), and the supported owning-domain respawn through
 * the driver's inherited vault_node spawn functions.
 *
 * A third assertion set (run_core_tests) exercises the Core:Entries
 * property-table marshal: the /lib/util/coercion codec round-trip
 * (values and refusals), a bare property-bearing clonable (obj/item)
 * storing and respawning through the default state root with no
 * per-app schema, the loud refusal of unencodable values at store
 * time, and the reserved-namespace filter (merry:* entries stay out
 * of the stored XML and out of default enumeration).
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
 * a no-op stub pending such a facility).
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/MyApp/lib/app";
inherit "/lib/util/named";
private inherit "/lib/util/coercion";

# define VAULT		"/usr/Vault/sys/vault"
# define SCHEMA		"/usr/Schema/sys/schema_daemon"
# define SCHEMA_NODE	"/usr/Schema/obj/schema_node"
# define INDEX		"/usr/Index/sys/index_daemon"
# define XML		"/usr/XML/sys/xml_daemon"

# define ROOT		"/usr/MyApp/data/things"
# define THING_PROG	"/usr/MyApp/obj/thing"
# define ITEM_PROG	"/usr/MyApp/obj/item"
# define DEMO_NAME	"MyApp:demo:thing1"
# define CONFIG_PROG	"/usr/MyApp/sys/config"
# define CONFIG_NAME	"MyApp:config:main"
/* where Vault->store lands CONFIG_NAME on disk: the Vault root plus the
 * colon-separated name with colons as directory separators. */
# define VAULT_FILE	"/usr/Vault/data/vault/MyApp/config/main.xml"
# define RESULT_FILE	"/usr/MyApp/data/test-result.log"

static void run_tests();
static void run_singleton_test();
static void run_xref_test();
static void run_core_tests();
static void run_codec_test();
static void run_core_roundtrip_test();
static void run_core_reject_test();
static void run_core_filter_test();
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
    node->add_attribute("peer", "lpc_obj", "query_peer");
    node->add_callback("set_label", "label");
    node->add_callback("set_count", "count");
    node->add_callback("set_peer", "peer");
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
    run_xref_test();
    run_core_tests();
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


static void run_xref_test()
{
    object alpha, beta, gamma, reloaded;

    /* phase 5: cross-object lpc_obj reference at application tier.
     * alpha's `peer` attribute points at beta; the stored XML carries
     * the literal OBJ(MyApp:xref:beta), and import resolves it through
     * Index (the name is not an LPC path) when beta is loaded. */
    alpha = clone_object(THING_PROG);
    alpha->set_object_name("MyApp:xref:alpha");
    alpha->set_label("alpha");
    alpha->set_count(1);

    beta = clone_object(THING_PROG);
    beta->set_object_name("MyApp:xref:beta");
    beta->set_label("beta");
    beta->set_count(2);

    alpha->set_peer(beta);

    catch {
	VAULT->store(alpha);
	VAULT->store(beta);
    } : {
	log_line("MyApp:test: FAIL: xref store threw");
	return;
    }

    destruct_object(alpha);		/* beta stays loaded */

    catch {
	VAULT->spawn_one_by_name("MyApp:xref:alpha");
    } : {
	log_line("MyApp:test: FAIL: xref respawn threw");
	return;
    }

    reloaded = find_named("MyApp:xref:alpha");
    if (!reloaded) {
	log_line("MyApp:test: FAIL: xref alpha not findable after respawn");
	return;
    }
    if (reloaded->query_peer() != beta) {
	log_line("MyApp:test: FAIL: xref peer did not resolve to the "
		 + "loaded beta");
	return;
    }
    if (reloaded->query_label() != "alpha" || reloaded->query_count() != 1) {
	log_line("MyApp:test: FAIL: xref alpha state mismatch");
	return;
    }

    log_line("MyApp:test: XREF OK");

    /* phase 6: dangling reference. A fresh driver-owned gamma stores
     * a peer reference to beta; with BOTH unloaded, importing gamma's
     * peer errors inside the Vault's configure step ("no object"),
     * which do_spawn catches internally. The create step already
     * succeeded, so gamma exists -- with whatever attributes imported
     * before the dangling one, and a nil peer. The assertion pins that
     * boundary: a dangling lpc_obj reference does not throw to the
     * spawn caller and leaves the object without its peer.
     *
     * gamma is used instead of re-destructing alpha because the
     * respawned alpha is OWNED BY THE VAULT DAEMON (clone_object ran
     * in the Vault's context during the respawn), so this driver
     * cannot destruct it -- kernel destruct_object is owner-gated.
     * Ownership of Vault-respawned clones follows the spawning
     * daemon, not the logical domain in the object's name. */
    gamma = clone_object(THING_PROG);
    gamma->set_object_name("MyApp:xref:gamma");
    gamma->set_label("gamma");
    gamma->set_count(3);
    gamma->set_peer(beta);

    catch {
	VAULT->store(gamma);
    } : {
	log_line("MyApp:test: FAIL: dangling store threw");
	return;
    }

    destruct_object(gamma);
    destruct_object(beta);

    catch {
	VAULT->spawn_one_by_name("MyApp:xref:gamma");
    } : {
	log_line("MyApp:test: FAIL: dangling respawn threw to caller");
	return;
    }

    reloaded = find_named("MyApp:xref:gamma");
    if (!reloaded) {
	log_line("MyApp:test: FAIL: dangling respawn did not create gamma");
	return;
    }
    if (reloaded->query_peer() != nil) {
	log_line("MyApp:test: FAIL: dangling peer unexpectedly resolved");
	return;
    }

    log_line("MyApp:test: XREF-DANGLING OK");
}


static void run_core_tests()
{
    run_codec_test();
    run_core_roundtrip_test();
    run_core_reject_test();
    run_core_filter_test();
}


static void run_codec_test()
{
    mixed *vals;
    mixed a, b;
    string enc;
    int i;

    /* phase 7: the coercion codec itself. Every encodable shape
     * round-trips exactly (canonical encoding compared after a
     * decode/encode cycle, plus value-level checks where == is
     * meaningful), and every refusal path refuses: aliased and cyclic
     * structures, light-weight objects, and malformed input. */
    vals = ({ 0, -42, 123456789,
	      0.1, -2.5, 1.0e10,
	      "", "line\nwith\ttabs \"quotes\" and \\slashes",
	      nil,
	      ({ }), ({ 1, "two", ({ 2.5, nil }) }),
	      ([ ]), ([ "k": ({ 1, 2 }), "m": ([ "x": 1 ]) ]),
	      this_object() });
    for (i = 0; i < sizeof(vals); i++) {
	enc = encodeValue(vals[i]);
	if (encodeValue(decodeValue(enc)) != enc) {
	    log_line("MyApp:test: FAIL: codec round-trip for " + enc);
	    return;
	}
    }
    if (decodeValue(encodeValue(0.1)) != 0.1) {
	log_line("MyApp:test: FAIL: codec float value drift");
	return;
    }
    if (decodeValue(encodeValue(this_object())) != this_object()) {
	log_line("MyApp:test: FAIL: codec object reference drift");
	return;
    }

    a = ({ 0 });
    a[0] = a;
    if (!catch(encodeValue(a))) {
	log_line("MyApp:test: FAIL: cyclic encode did not throw");
	return;
    }
    b = ({ 1 });
    if (!catch(encodeValue(({ b, b })))) {
	log_line("MyApp:test: FAIL: aliased encode did not throw");
	return;
    }
    if (!catch(encodeValue(XML->parse("<a>b</a>")))) {
	log_line("MyApp:test: FAIL: light-weight encode did not throw");
	return;
    }

    if (!catch(decodeValue("({ 1")) ||
	!catch(decodeValue("\"open")) ||
	!catch(decodeValue("<MyApp:no:such:object>")) ||
	!catch(decodeValue("5 x")) ||
	!catch(decodeValue(""))) {
	log_line("MyApp:test: FAIL: malformed decode did not throw");
	return;
    }

    log_line("MyApp:test: CODEC OK");
}


static void run_core_roundtrip_test()
{
    object item, peer, reloaded;

    /* phase 8: the Core:Entries property-table marshal. A bare
     * property-bearing clonable (obj/item, default state root) stores
     * mixed-shape property values and a fresh respawn restores every
     * one -- no per-app schema involved. */
    item = clone_object(ITEM_PROG);
    item->set_object_name("MyApp:core:item1");
    peer = clone_object(ITEM_PROG);
    peer->set_object_name("MyApp:core:peer1");

    item->set_property("core:num", 42);
    item->set_property("core:ratio", 0.1);
    item->set_property("core:text", "hi \"there\"\nline2\ttab");
    item->set_property("core:peer", peer);
    item->set_property("core:list", ({ 1, 2.5, "three" }));
    item->set_property("core:map", ([ "a": ({ 1, 2 }), "b": "two" ]));

    catch {
	VAULT->store(item);
    } : {
	log_line("MyApp:test: FAIL: core store threw");
	return;
    }
    destruct_object(item);

    catch {
	VAULT->spawn_one_by_name("MyApp:core:item1");
    } : {
	log_line("MyApp:test: FAIL: core respawn threw");
	return;
    }
    reloaded = find_named("MyApp:core:item1");
    if (!reloaded) {
	log_line("MyApp:test: FAIL: core item not findable after respawn");
	return;
    }
    if (reloaded->query_property("core:num") != 42 ||
	reloaded->query_property("core:ratio") != 0.1 ||
	reloaded->query_property("core:text") != "hi \"there\"\nline2\ttab" ||
	reloaded->query_property("core:peer") != peer) {
	log_line("MyApp:test: FAIL: core scalar round-trip mismatch");
	return;
    }
    if (encodeValue(reloaded->query_property("core:list")) !=
				encodeValue(({ 1, 2.5, "three" })) ||
	encodeValue(reloaded->query_property("core:map")) !=
			encodeValue(([ "a": ({ 1, 2 }), "b": "two" ]))) {
	log_line("MyApp:test: FAIL: core container round-trip mismatch");
	return;
    }
    log_line("MyApp:test: CORE ROUND-TRIP OK");
}


static void run_core_reject_test()
{
    object item;
    mixed a, b;

    /* phase 9: unencodable property values refuse loudly at store
     * time -- the whole export aborts rather than writing a lossy
     * file. Cyclic, aliased, and light-weight values each throw. */
    item = clone_object(ITEM_PROG);
    item->set_object_name("MyApp:core:item2");

    a = ({ 0 });
    a[0] = a;
    item->set_property("core:bad", a);
    if (!catch(VAULT->store(item))) {
	log_line("MyApp:test: FAIL: cyclic store did not throw");
	return;
    }
    b = ({ 1 });
    item->set_property("core:bad", ({ b, b }));
    if (!catch(VAULT->store(item))) {
	log_line("MyApp:test: FAIL: aliased store did not throw");
	return;
    }
    item->set_property("core:bad", XML->parse("<a>b</a>"));
    if (!catch(VAULT->store(item))) {
	log_line("MyApp:test: FAIL: light-weight store did not throw");
	return;
    }
    item->clear_property("core:bad");
    log_line("MyApp:test: CORE ENCODE-REJECT OK");
}


static void run_core_filter_test()
{
    object item, reloaded;
    string text;
    string *keys;

    /* phase 10: the reserved-namespace filter. merry:* entries
     * (observer slots, scripts) stay out of default enumeration and
     * out of the stored XML; the opaque flag still reaches them; app
     * state crosses the marshal and the merry: entries do not. The
     * entries are planted with set_raw_property -- the documented
     * bypass -- because the dispatch path gates merry:* writes behind
     * the registration capability, which is exactly what the filter
     * spares the marshal path from on re-import. */
    item = clone_object(ITEM_PROG);
    item->set_object_name("MyApp:core:item3");
    item->set_property("core:keep", 1);
    item->set_raw_property("merry:lib:probe", "$(\"noop\")");
    item->set_raw_property("merry:on:core.sig:main", ({ }));

    keys = item->query_property_indices();
    if (sizeof(keys & ({ "merry:lib:probe", "merry:on:core.sig:main" }))) {
	log_line("MyApp:test: FAIL: default enumeration leaked merry: keys");
	return;
    }
    keys = item->query_property_indices(1);
    if (sizeof(keys & ({ "merry:lib:probe", "merry:on:core.sig:main" }))
								    != 2) {
	log_line("MyApp:test: FAIL: opaque enumeration missing merry: keys");
	return;
    }

    catch {
	VAULT->store(item);
    } : {
	log_line("MyApp:test: FAIL: filtered store threw");
	return;
    }
    text = read_file("/usr/Vault/data/vault/MyApp/core/item3.xml");
    if (!text) {
	log_line("MyApp:test: FAIL: filtered store XML not readable");
	return;
    }
    if (sscanf(text, "%*smerry:") != 0) {
	log_line("MyApp:test: FAIL: stored XML carries merry: entries");
	return;
    }

    destruct_object(item);
    catch {
	VAULT->spawn_one_by_name("MyApp:core:item3");
    } : {
	log_line("MyApp:test: FAIL: filtered respawn threw");
	return;
    }
    reloaded = find_named("MyApp:core:item3");
    if (!reloaded) {
	log_line("MyApp:test: FAIL: filtered item not findable");
	return;
    }
    if (reloaded->query_property("core:keep") != 1) {
	log_line("MyApp:test: FAIL: filtered app state lost");
	return;
    }
    if (reloaded->query_raw_property("merry:lib:probe") != nil) {
	log_line("MyApp:test: FAIL: merry: entry crossed the marshal");
	return;
    }
    log_line("MyApp:test: CORE FILTER OK");
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
