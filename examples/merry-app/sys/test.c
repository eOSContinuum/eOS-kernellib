/*
 * Boot-time test driver for the Merry application example.
 *
 * Exercises the LM-3 + LM-3.5 lift end-to-end via two assertions:
 *
 *   1. Ancestry-walk + invocation. A 2-generation Hierarchy
 *      (parent <- child) is built via set_ur_object. A Merry script
 *      is bound to the parent under "merry:lib:greet"; run_merry is
 *      called on the child; find_merry walks query_ur_object() up to
 *      the parent, finds the script, evaluates it. The expected
 *      return value is the literal "MerryApp:test: ANCESTRY OK".
 *
 *   2. Sandbox-firing. A second Merry script attempts to call
 *      clone_object, which is in the SANDBOX(f) deny list inside
 *      merrynode.c. The call must error with "function 'clone_object'
 *      not allowed in merry code".
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/MerryApp/data/test-result.log (same convention as the vault-app
 * example -- no kernel-layer log facility lifted yet per LV-4.5b).
 *
 * Phases are wrapped in catch{} so a failure in phase 1 does not mask
 * a different failure in phase 2; both fail-paths log a distinct
 * sentinel that the verify command in README.md can grep for.
 *
 * Deferral notes:
 *   - Duplicate / Spawn (clone_object inside merrynode) are not
 *     exercised here. Both call clone_object directly; the SANDBOX
 *     shadow may capture the call from inside Spawn / Duplicate too,
 *     making them broken without ::clone_object escape. That fix is
 *     L8 cascade work for LM-6 when a richer Merry test exercises
 *     them.
 *   - $delay() / do_delay() / mcontext.c::create(4-arg) is not
 *     exercised here. Same LM-6 territory.
 *   - script-space resolution (LabelCall / LabelRef) is not exercised
 *     here. A separate test driver registering a script-space handler
 *     would cover it.
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

# define MERRY_DATA	"/usr/Merry/data/merry"
# define THING_PROG	"/usr/MerryApp/obj/thing"
# define RESULT_FILE	"/usr/MerryApp/data/test-result.log"

static void run_tests();
private void log_line(string msg);


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
    object script, bad_script;
    mixed result;

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
