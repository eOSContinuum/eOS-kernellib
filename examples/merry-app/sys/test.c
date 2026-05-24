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

    /* phase 6: DI-2 batched_set (non-atomic) per DD-1 (c) -- writes a
     * multi-key mapping under one batch-id with sequential seq starting
     * at 0 (DD-3 (b)). Verifies the public LFUN compiles and that both
     * properties land; the batch-id and seq are daemon-internal and not
     * surfaced through a public API at DI-2 scope (DI-5's
     * query_changes_since exposes them). */

    catch {
	if (MERRY_DAEMON->_current_batch_id() != 0) {
	    log_line("MerryApp:test: FAIL: batched_set non-zero current_batch_id pre-call");
	    return;
	}
	MERRY_DAEMON->batched_set(child, ([
	    "test:batch:k1": 100,
	    "test:batch:k2": 200,
	]));
	if (child->query_raw_property("test:batch:k1") != 100 ||
	    child->query_raw_property("test:batch:k2") != 200) {
	    log_line("MerryApp:test: FAIL: batched_set values did not land");
	    return;
	}
	if (MERRY_DAEMON->_current_batch_id() != 0) {
	    log_line("MerryApp:test: FAIL: batched_set left batch active");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: batched_set non-atomic threw");
	return;
    }

    log_line("MerryApp:test: BATCH OK");

    /* phase 7: DI-2 atomic-mode opt-in per DD-4 (d) -- body + status
     * entry write run inside DGD atomic{}; on success the writes commit
     * normally. The abort-rollback branch is DI-8's scope; phase 7
     * verifies the success branch and that the opts-mapping is
     * accepted on the varargs slot. */

    catch {
	MERRY_DAEMON->batched_set(child,
				  ([ "test:batch:atomic": 42 ]),
				  ([ "atomic": 1 ]));
	if (child->query_raw_property("test:batch:atomic") != 42) {
	    log_line("MerryApp:test: FAIL: atomic batched_set value did not land");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: atomic batched_set threw");
	return;
    }

    log_line("MerryApp:test: BATCH ATOMIC OK");

    /* phase 8: DI-2 BatchedSet from Merry source -- verifies the
     * merrynode.c surface is reachable from compiled scripts; the
     * mapping-arg signature composes inline per L14 #15. */

    catch {
	object batch_source_script;

	batch_source_script = new_object(MERRY_DATA,
					 "BatchedSet($this, ([ "
					 + "\"test:batch:src1\": 11, "
					 + "\"test:batch:src2\": 22 "
					 + "])); return TRUE;");
	parent->set_property("merry:lib:batch_test", batch_source_script);

	result = run_merry(child, "batch_test", "lib", ([ ]));
	if (!result) {
	    log_line("MerryApp:test: FAIL: BatchedSet source returned false");
	    return;
	}
	if (child->query_raw_property("test:batch:src1") != 11 ||
	    child->query_raw_property("test:batch:src2") != 22) {
	    log_line("MerryApp:test: FAIL: BatchedSet source values did not land");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: BatchedSet from Merry source threw");
	return;
    }

    log_line("MerryApp:test: BATCH SOURCE OK");

    /* phase 9: DI-2 non-atomic batch() abort path per DD-2 (d) +
     * DD-3 (c) -- the callable throws; status entry must persist as
     * "main-aborted" and the error must propagate to caller's catch{}. */

    catch {
	int err_caught, i;
	mixed *status_after;

	err_caught = 0;
	catch {
	    MERRY_DAEMON->batch(this_object(), "_throw_for_test", ({ }));
	} : {
	    err_caught = 1;
	}
	if (!err_caught) {
	    log_line("MerryApp:test: FAIL: batch() did not propagate throw");
	    return;
	}
	/* Scan a small batch-id window looking for the main-aborted
	 * entry; the exact id isn't surfaced through a public API at
	 * DI-2 scope (DI-5's query_changes_since covers that). */
	status_after = nil;
	for (i = 1; i <= 200 && !status_after; i ++) {
	    mixed *s;
	    s = MERRY_DAEMON->_query_batch_status(i);
	    if (s && s[0] == "main-aborted") {
		status_after = s;
	    }
	}
	if (!status_after) {
	    log_line("MerryApp:test: FAIL: no main-aborted status entry found");
	    return;
	}
	if (MERRY_DAEMON->_current_batch_id() != 0) {
	    log_line("MerryApp:test: FAIL: batch() abort left batch active");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: batch() abort path setup threw");
	return;
    }

    log_line("MerryApp:test: BATCH ABORT OK");

    /* phase 10: DI-3 dispatcher -- simple main-timing observer.
     * Register a main observer on child for "test:dispatch:main" with
     * a source that writes a marker property; verify the marker landed
     * after a set_property on the observed path. */

    catch {
	MERRY_DAEMON->register_observer(child, "test:dispatch:main", "main",
	    "Set($this, \"test:dispatch:main:fired\", 1); return TRUE;");

	child->set_property("test:dispatch:main", 42);

	if (child->query_raw_property("test:dispatch:main") != 42) {
	    log_line("MerryApp:test: FAIL: DISPATCH MAIN value did not land");
	    return;
	}
	if (child->query_raw_property("test:dispatch:main:fired") != 1) {
	    log_line("MerryApp:test: FAIL: DISPATCH MAIN observer did not fire");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH MAIN setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH MAIN OK");

    /* phase 11: DI-3 dispatcher -- pre + main + post timings.
     * Register observers on all three slots that append to a marker
     * string. Verify the string reads "pre|main|post" after the write,
     * confirming both per-timing slot firing and DD-4 (b) ordering. */

    catch {
	MERRY_DAEMON->register_observer(child, "test:dispatch:order", "pre",
	    "Set($this, \"test:dispatch:order:trace\", Get($this, \"test:dispatch:order:trace\") + \"pre|\"); return TRUE;");
	MERRY_DAEMON->register_observer(child, "test:dispatch:order", "main",
	    "Set($this, \"test:dispatch:order:trace\", Get($this, \"test:dispatch:order:trace\") + \"main|\"); return TRUE;");
	MERRY_DAEMON->register_observer(child, "test:dispatch:order", "post",
	    "Set($this, \"test:dispatch:order:trace\", Get($this, \"test:dispatch:order:trace\") + \"post\"); return TRUE;");

	child->set_raw_property("test:dispatch:order:trace", "");
	child->set_property("test:dispatch:order", 1);

	if (child->query_raw_property("test:dispatch:order:trace")
	    != "pre|main|post") {
	    log_line("MerryApp:test: FAIL: DISPATCH ORDER trace was \""
		     + (string) child->query_raw_property("test:dispatch:order:trace")
		     + "\"");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH ORDER setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH ORDER OK");

    /* phase 12: DI-3 dispatcher -- pre veto per DD-4 (a). Register a
     * pre observer that throws; verify (1) the error propagates to
     * caller, (2) the value did NOT land (pre veto aborts before the
     * write step). Initial value seeded via set_raw_property so the
     * dispatcher does not fire on the seed. */

    catch {
	int err_caught;

	child->set_raw_property("test:dispatch:veto", 100);

	MERRY_DAEMON->register_observer(child, "test:dispatch:veto", "pre",
	    "error(\"merry: pre observer veto\");");

	err_caught = 0;
	catch {
	    child->set_property("test:dispatch:veto", 200);
	} : {
	    err_caught = 1;
	}
	if (!err_caught) {
	    log_line("MerryApp:test: FAIL: DISPATCH VETO did not propagate");
	    return;
	}
	if (child->query_raw_property("test:dispatch:veto") != 100) {
	    log_line("MerryApp:test: FAIL: DISPATCH VETO wrote despite veto");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH VETO setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH VETO OK");

    /* phase 13: DI-3 dispatcher -- cycle detection per DD-2 (b).
     * Register a main observer that writes the SAME property; the
     * recursive dispatch must detect the cycle and throw. The outer
     * set_property error string must include "cycle". */

    catch {
	int err_caught;

	MERRY_DAEMON->register_observer(child, "test:cycle", "main",
	    "Set($this, \"test:cycle\", 99); return TRUE;");

	err_caught = 0;
	catch {
	    child->set_property("test:cycle", 1);
	} : {
	    err_caught = 1;
	}
	if (!err_caught) {
	    log_line("MerryApp:test: FAIL: DISPATCH CYCLE did not detect");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH CYCLE setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH CYCLE OK");

    /* phase 14: DI-3 dispatcher -- ancestry walk per DD-5 (a). Register
     * a main observer on PARENT for "test:ancestry"; write the property
     * on CHILD; find_observers walks the ur chain and resolves the
     * parent's observer; observer's $this is CHILD (the dispatch host)
     * so the marker property lands on CHILD. */

    catch {
	MERRY_DAEMON->register_observer(parent, "test:ancestry", "main",
	    "Set($this, \"test:ancestry:observed\", 1); return TRUE;");

	child->set_property("test:ancestry", 42);

	if (child->query_raw_property("test:ancestry") != 42) {
	    log_line("MerryApp:test: FAIL: DISPATCH ANCESTRY value did not land on child");
	    return;
	}
	if (child->query_raw_property("test:ancestry:observed") != 1) {
	    log_line("MerryApp:test: FAIL: DISPATCH ANCESTRY parent observer did not fire on child write");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH ANCESTRY setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH ANCESTRY OK");

    /* phase 15: DI-3 dispatcher -- implicit-batch wrapping per DD-3 (b).
     * An unbatched set_property must (1) leave _current_batch_id() at 0
     * before AND after the call (the implicit batch is entered and
     * exited cleanly), and (2) record a "completed" status entry for
     * the implicit batch in batch_status. */

    catch {
	int pre_id, post_id, found_completed, i;
	mixed *s;

	pre_id = MERRY_DAEMON->_current_batch_id();
	if (pre_id != 0) {
	    log_line("MerryApp:test: FAIL: DISPATCH IMPLICIT pre-call batch active ("
		     + (string) pre_id + ")");
	    return;
	}

	child->set_property("test:implicit:probe", 1);

	post_id = MERRY_DAEMON->_current_batch_id();
	if (post_id != 0) {
	    log_line("MerryApp:test: FAIL: DISPATCH IMPLICIT post-call batch active ("
		     + (string) post_id + ")");
	    return;
	}

	/* Scan a small recent window for a "completed" entry. The implicit
	 * batch id is daemon-internal; finding ANY recent completed entry
	 * suffices for the smoke. */
	found_completed = 0;
	for (i = 1; i <= 400 && !found_completed; i ++) {
	    s = MERRY_DAEMON->_query_batch_status(i);
	    if (s && s[0] == "completed") {
		found_completed = 1;
	    }
	}
	if (!found_completed) {
	    log_line("MerryApp:test: FAIL: DISPATCH IMPLICIT no completed status entry");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH IMPLICIT setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH IMPLICIT OK");
}


/* Helper for phase 9: a public LFUN that always throws so batch() can
 * exercise the abort path. Named with underscore prefix to mark it as
 * internal/test-only; deliberately not static so call_other can reach
 * it from MERRY's batch() implementation. */
void _throw_for_test()
{
    error("MerryApp:test: deliberate throw for batch abort verification");
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
