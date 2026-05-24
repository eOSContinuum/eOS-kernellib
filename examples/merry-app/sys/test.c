/*
 * Boot-time test driver for the Merry application example.
 *
 * Exercises the LM-3 + LM-3.5 + LM-6 lift end-to-end via five
 * assertions:
 *
 *   1. Ancestry-walk + invocation. A 2-generation Hierarchy
 *      (parent <- child) is built via set_ur_object. A Merry script
 *      is bound to the parent under "merry:lib:greet"; run_merry is
 *      called on the child; find_merry walks query_ur_object() up to
 *      the parent, finds the script, evaluates it. The expected
 *      return value is the literal "MerryApp:test: ANCESTRY OK".
 *
 *   2. Sandbox-firing. A Merry script attempts to call
 *      clone_object, which is in the SANDBOX(f) deny list inside
 *      merrynode.c. The call must error with "function 'clone_object'
 *      not allowed in merry code".
 *
 *   3. Spawn merryfun (LM-6 sub-task a). A Merry script invokes the
 *      Spawn() static merryfun on the binding host; Spawn calls
 *      ::clone_object to escape the local SANDBOX(clone_object)
 *      shadow, clones the binding host's clonable, and sets the
 *      ur-object on the new clone. The script returns the spawned
 *      clone's object_name; the test driver verifies it begins with
 *      the parent's clonable path and includes a clone suffix.
 *      Duplicate() uses the same ::clone_object escape but is not
 *      exercised here because it requires a Schema-registered state
 *      model (export_state walks SID->get_root_node(ob)). The fix
 *      applies to both merryfuns; the test covers the more
 *      schema-free of the two.
 *
 *   4. $delay() / mcontext.c::create 4-arg dispatch (LM-6 sub-task b).
 *      A Merry script issues `$delay(1, FALSE);` then sets a marker
 *      property on the binding host. The first call returns FALSE
 *      synchronously and schedules a continuation via the binding
 *      host's `delayed_call` LFUN (provided by obj/thing.c). One
 *      second later, perform_delayed_call fires
 *      merry_continuation on the mcontext LWO, which calls
 *      run_merries to resume execution at the case label; the
 *      Set($this, "delay_fired", 1) statement then executes. A
 *      verify_delay call_out checks the property two seconds after
 *      test setup completes.
 *
 *   5. LabelCall / LabelRef script-space resolution (LM-6 sub-task c).
 *      The test driver registers itself as the "testspace" script
 *      space via MERRY->register_script_space; this object's
 *      query_method / call_method LFUNs (below) constitute the
 *      handler protocol that merrynode.c::Call walks. A Merry script
 *      invokes `testspace::greet($who: "world");` which compiles to
 *      LabelCall("testspace", "greet", ({"who", "world"})); the
 *      script returns "Hello world".
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/MerryApp/data/test-result.log (same convention as the vault-app
 * example -- no kernel-layer log facility lifted yet per LV-4.5b).
 *
 * Phases are wrapped in catch{} so a failure in one does not mask a
 * different failure in another; both fail-paths log a distinct
 * sentinel that the verify command in README.md can grep for.
 *
 * The test driver doubles as the script-space handler for phase 5.
 * In production a handler would typically be a distinct object so
 * its role and lifecycle are clear; collapsing them here keeps the
 * example to a single file under sys/.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/MerryApp/lib/app";
inherit "/lib/util/named";

/* Merry's invocation API is a static surface in /usr/Merry/lib/merryapi.
 * Daemons that want to dispatch a Merry script inherit it (rather than
 * call the SYS_MERRY daemon via ->run_merry) -- the static methods
 * resolve via the inherit chain at compile time and avoid the
 * external-call overhead of an LFUN dispatch on every invocation. This
 * is the SkotOS convention and what merryapi.c was authored against
 * (every find_merry / find_merry_location / run_merry / find_merries /
 * run_merries call site in production SkotOS goes through an
 * inheriting daemon, not through SYS_MERRY directly). */
inherit "/usr/Merry/lib/merryapi";

# define MERRY_DAEMON	"/usr/Merry/sys/merry"
# define MERRY_DATA	"/usr/Merry/data/merry"
# define THING_PROG	"/usr/MerryApp/obj/thing"
# define RESULT_FILE	"/usr/MerryApp/data/test-result.log"

static void run_tests();
static void verify_delay(object child);
private void log_line(string msg);

object delay_target;	/* binding host for phase 4; checked by verify_delay */


static void create()
{
    /* Defer to call_out so System/initd has finished its load loop;
     * MerryApp's initd fires alphabetically before Merry's, so a
     * direct call into MERRY at create() time would find the daemon
     * not-yet-compiled. The call_out fires once System/initd::create()
     * has returned, by which point every per-domain initd has run. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    set_object_name("MerryApp:TestDriver");
    /* /usr/MerryApp/data/ may not exist on first boot. */
    catch(make_dir("/usr/MerryApp/data"));
    catch(remove_file(RESULT_FILE));
    run_tests();
}


static void run_tests()
{
    object parent, child;
    object script, bad_script, spawn_script, delay_script, label_script;
    mixed result;
    string spawn_name;

    log_line("MerryApp:test: starting");

    /* phase 1: ancestry-walk invocation */

    parent = clone_object(THING_PROG);
    parent->set_object_name("MerryApp:demo:parent");

    child = clone_object(THING_PROG);
    child->set_object_name("MerryApp:demo:child");
    child->set_ur_object(parent);

    catch {
	script = new_object(MERRY_DATA,
			    "return \"MerryApp:test: ANCESTRY OK\";");
    } : {
	log_line("MerryApp:test: FAIL: parse_merry/compile threw");
	return;
    }

    parent->set_property("merry:lib:greet", script);

    catch {
	result = run_merry(child, "greet", "lib", ([ ]));
    } : {
	log_line("MerryApp:test: FAIL: run_merry threw");
	return;
    }

    if (typeof(result) != T_STRING) {
	log_line("MerryApp:test: FAIL: result was non-string (typeof="
		 + (string) typeof(result) + ")");
	return;
    }
    if (result != "MerryApp:test: ANCESTRY OK") {
	log_line("MerryApp:test: FAIL: result \"" + result + "\"");
	return;
    }

    log_line("MerryApp:test: ANCESTRY OK");

    /* phase 2: sandbox-firing */

    catch {
	bad_script = new_object(MERRY_DATA,
				"clone_object(\"/foo/bar\");");
    } : {
	log_line("MerryApp:test: FAIL: sandbox-script compile threw");
	return;
    }

    parent->set_property("merry:lib:badcall", bad_script);

    catch {
	run_merry(child, "badcall", "lib", ([ ]));
	log_line("MerryApp:test: FAIL: sandbox did not fire");
	return;
    } : {
	log_line("MerryApp:test: SANDBOX OK");
    }

    /* phase 3: Spawn merryfun -- exercises ::clone_object escape past
     * the local SANDBOX(clone_object) shadow inside merrynode. The
     * Merry source receives the binding host via $this; Spawn returns
     * a fresh clone whose object_name carries the binding host's
     * clonable path. */

    catch {
	/* Merry source has no LPC-style variable declarations; it is
	 * an expression / statement language closer to TCL than C.
	 * Compose the call inline and let LPC's return propagate. */
	spawn_script = new_object(MERRY_DATA,
				  "return object_name(Spawn($this));");
    } : {
	log_line("MerryApp:test: FAIL: spawn-script compile threw");
	return;
    }

    parent->set_property("merry:lib:make_one", spawn_script);

    catch {
	result = run_merry(child, "make_one", "lib", ([ ]));
    } : {
	log_line("MerryApp:test: FAIL: Spawn threw");
	return;
    }

    if (typeof(result) != T_STRING) {
	log_line("MerryApp:test: FAIL: Spawn returned non-string (typeof="
		 + (string) typeof(result) + ")");
	return;
    }
    if (!sscanf(result, THING_PROG + "#%*d", spawn_name)) {
	log_line("MerryApp:test: FAIL: spawned object name \""
		 + result + "\" does not match " + THING_PROG + "#NN");
	return;
    }

    log_line("MerryApp:test: SPAWN OK");

    /* phase 4: $delay() -- schedules a continuation via the binding
     * host's delayed_call LFUN. First call returns FALSE
     * synchronously; one second later, perform_delayed_call fires
     * merry_continuation on the mcontext LWO, which calls run_merries
     * to resume execution and Set delay_fired to 1. verify_delay
     * call_out below polls the property at t=2. */

    catch {
	delay_script = new_object(MERRY_DATA,
				  "$delay(1, FALSE); " +
				  "Set($this, \"delay_fired\", 1); " +
				  "return TRUE;");
    } : {
	log_line("MerryApp:test: FAIL: delay-script compile threw");
	return;
    }

    parent->set_property("merry:lib:delay_test", delay_script);

    catch {
	result = run_merry(child, "delay_test", "lib", ([ ]));
    } : {
	log_line("MerryApp:test: FAIL: $delay first call threw");
	return;
    }

    delay_target = child;
    call_out("verify_delay", 2, child);

    /* phase 5: LabelCall / LabelRef -- register this object as the
     * "testspace" script space; merrynode.c::Call walks
     * query_method / call_method (below) when it dispatches a
     * LabelCall. */

    catch {
	MERRY_DAEMON->register_script_space("testspace", this_object());
    } : {
	log_line("MerryApp:test: FAIL: register_script_space threw");
	return;
    }

    catch {
	label_script = new_object(MERRY_DATA,
				  "return testspace::greet($who: \"world\");");
    } : {
	log_line("MerryApp:test: FAIL: labelcall-script compile threw");
	return;
    }

    parent->set_property("merry:lib:label_test", label_script);

    catch {
	result = run_merry(child, "label_test", "lib", ([ ]));
    } : {
	log_line("MerryApp:test: FAIL: LabelCall threw");
	return;
    }

    if (typeof(result) != T_STRING || result != "Hello world") {
	log_line("MerryApp:test: FAIL: LabelCall returned \""
		 + (typeof(result) == T_STRING ? result : "(non-string)")
		 + "\"");
	return;
    }

    log_line("MerryApp:test: LABELCALL OK");
}


static void verify_delay(object child)
{
    int fired;

    if (!child) {
	log_line("MerryApp:test: FAIL: delay binding-host destructed before verify");
	return;
    }
    fired = child->query_raw_property("delay_fired");
    if (fired == 1) {
	log_line("MerryApp:test: DELAY OK");
    } else {
	log_line("MerryApp:test: FAIL: delay continuation did not fire ("
		 + (string) fired + ")");
    }
}


/* script-space handler protocol; phase 5 above registers this object
 * as "testspace" so merrynode.c::Call routes a LabelCall to these
 * LFUNs. query_method returns truthy when the named method exists;
 * call_method executes and returns a value. */

int query_method(string name)
{
    return name == "greet";
}

mixed call_method(string name, mapping args)
{
    if (name == "greet") {
	return "Hello " + args["who"];
    }
    error("unknown method: " + name);
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
