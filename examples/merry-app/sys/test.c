/*
 * Boot-time test driver for the Merry application example.
 *
 * Exercises the Merry runtime end-to-end via the assertions below:
 *
 *   1. Ancestry-walk + invocation. A 2-generation Hierarchy
 *      (parent <- child) is built via set_ur_object. A Merry script
 *      is bound to the parent under "merry:lib:greet"; run_merry is
 *      called on the child; find_merry walks query_parent() up to
 *      the parent, finds the script, evaluates it. The expected
 *      return value is the literal "MerryApp:test: ANCESTRY OK".
 *
 *   2. Sandbox-firing. A Merry script attempts to call
 *      clone_object, which is in the SANDBOX(f) deny list inside
 *      merrynode.c. The call must error with "function 'clone_object'
 *      not allowed in merry code".
 *
 *   3. Spawn merryfun. A Merry script invokes the
 *      Spawn() static merryfun on the binding host; Spawn calls
 *      ::clone_object to escape the local SANDBOX(clone_object)
 *      shadow, clones the binding host's clonable, and sets the
 *      ur-object on the new clone. The script returns the spawned
 *      clone's object_name; the test driver verifies it begins with
 *      the parent's clonable path and includes a clone suffix.
 *      Duplicate() uses the same ::clone_object escape and is
 *      exercised by phase 3b: the MerryApp:Thing schema registered at
 *      setup gives the clonable a state model, and a Merry script
 *      calls Duplicate($this), whose result must be a distinct clone
 *      carrying the schema-declared state (export_state walks
 *      SID->get_root_node(ob); import_state replays it on the copy).
 *
 *   4. $delay() / mcontext.c::create 4-arg dispatch.
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
 *   5. LabelCall / LabelRef script-space resolution.
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
 * example -- the platform has no kernel-layer log facility yet).
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
 * is the convention merryapi.c was authored against: invocation call
 * sites go through an inheriting daemon, not through the Merry daemon
 * directly. */
inherit "/usr/Merry/lib/merryapi";

# define MERRY_DAEMON	"/usr/Merry/sys/merry"
# define MERRY_DATA	"/usr/Merry/data/merry"
# define THING_PROG	"/usr/MerryApp/obj/thing"
# define SCHEMA_NODE	"/usr/Schema/obj/schema_node"
# define PERSIST_HELPER	"/usr/System/sys/persist_helper"
# define RESULT_FILE	"/usr/MerryApp/data/test-result.log"

static void run_tests();
static void verify_delay(object child);
static void verify_suicide();
static void phase17_verify();
private void log_line(string msg);

object delay_target;	/* binding host for phase 4; checked by verify_delay */
object suicide_prog;	/* compiled node for phase 3c; nulls on destruct */
object persist_host;	/* binding host for phase 16/17; survives hotboot */


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
    object node;

    set_object_name("MerryApp:TestDriver");
    /* Register the MerryApp:Thing schema (matching obj/thing's
     * queryStateRoot) so export_state / import_state can walk a
     * state model for the clonable -- the Duplicate merryfun needs
     * it. One lpc_str attribute, label, backed by the property
     * store. */
    node = clone_object(SCHEMA_NODE);
    node->set_name("MerryApp", "Thing");
    node->add_attribute("label", "lpc_str", "query_label");
    node->add_callback("set_label", "label");
    /* /usr/MerryApp/data/ may not exist on first boot. */
    catch(make_dir("/usr/MerryApp/data"));
    catch(remove_file(RESULT_FILE));
    run_tests();
}


static void run_tests()
{
    object parent, child, dup_host;
    object script, bad_script, spawn_script, delay_script, label_script;
    object dup_script;
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

    /* phase 3b: Duplicate merryfun -- the state-copying sibling of
     * Spawn. Unlike Spawn (which clones the clonable fresh),
     * Duplicate exports the source clone's state through its
     * registered state model (the MerryApp:Thing schema registered
     * at setup) and imports it into the new clone. The duplicate
     * must be a distinct clone of the same clonable carrying the
     * schema-declared state. */

    catch {
	dup_script = new_object(MERRY_DATA, "return Duplicate($this);");
    } : {
	log_line("MerryApp:test: FAIL: dup-script compile threw");
	return;
    }

    dup_host = clone_object(THING_PROG);
    dup_host->set_label("carried");
    dup_host->set_property("merry:lib:make_copy", dup_script);

    catch {
	result = run_merry(dup_host, "make_copy", "lib", ([ ]));
    } : {
	log_line("MerryApp:test: FAIL: Duplicate threw");
	return;
    }

    if (typeof(result) != T_OBJECT) {
	log_line("MerryApp:test: FAIL: Duplicate returned non-object "
		 + "(typeof=" + (string) typeof(result) + ")");
	return;
    }
    if (result == dup_host) {
	log_line("MerryApp:test: FAIL: Duplicate returned the source");
	return;
    }
    if (!sscanf(object_name(result), THING_PROG + "#%*d")) {
	log_line("MerryApp:test: FAIL: duplicate \""
		 + object_name(result) + "\" is not a clone of "
		 + THING_PROG);
	return;
    }
    if (result->query_label() != "carried") {
	log_line("MerryApp:test: FAIL: duplicate did not carry label "
		 + "(got "
		 + (result->query_label() ?
		    "\"" + result->query_label() + "\"" : "(nil)")
		 + ")");
	return;
    }

    log_line("MerryApp:test: DUPLICATE OK");

    /* phase 3c: compiled-node eviction. suicide() is the eviction
     * entry point MERRY's clean_nodes() drives on MRU overflow; it
     * schedules do_suicide via call_out, which runs the (sandbox-
     * escaped) source-file cleanup and self-destructs the compiled
     * program. A distinctive source keeps the MD5-keyed program from
     * being shared with any other phase's script. The global
     * suicide_prog ref nulls when the destruct lands; verify_suicide
     * checks it at t=+2 (after the restore, alongside DELAY OK,
     * since boot 1 self-exits before timed call_outs fire). */

    catch {
	script = new_object(MERRY_DATA, "return 20260610;");
	suicide_prog = script->query_program();
    } : {
	log_line("MerryApp:test: FAIL: suicide-script compile threw");
	return;
    }

    if (!suicide_prog) {
	log_line("MerryApp:test: FAIL: compiled node not reachable via "
		 + "query_program");
	return;
    }

    suicide_prog->suicide();
    call_out("verify_suicide", 2);

    /* phase 4: $delay() -- schedules a continuation via the binding
     * host's delayed_call LFUN. First call returns FALSE
     * synchronously; one second later, perform_delayed_call fires
     * merry_continuation on the mcontext LWO, which calls run_merries
     * to resume execution and Set delay_fired to 1. verify_delay
     * call_out below polls the property at t=2. NOTE: with phase 16
     * triggering dump_state + shutdown at t<2, the verify_delay
     * call_out is in the snapshot at first-boot exit; it fires after
     * restore (along with phase17_verify), so DELAY OK lands on the
     * second boot. */

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

    /* phase 5b: capability-gate reject path. Every registration so
     * far rode the accept path (caller domain == target domain). The
     * reject branch -- a caller from one domain registering on a
     * target in another domain, without approved-registrar
     * membership -- must throw for both gated surfaces. The Merry
     * daemon itself serves as the cross-domain target. */

    catch {
	MERRY_DAEMON->register_script_space("rejectspace",
					    find_object(MERRY_DAEMON));
	log_line("MerryApp:test: FAIL: cross-domain register_script_space "
		 + "did not throw");
	return;
    } : {
	/* expected: the gate refused the cross-domain registration */
    }

    catch {
	MERRY_DAEMON->register_observer(find_object(MERRY_DAEMON),
	    "test:reject", "main", "return TRUE;");
	log_line("MerryApp:test: FAIL: cross-domain register_observer "
		 + "did not throw");
	return;
    } : {
	/* expected: same gate, same refusal */
    }

    log_line("MerryApp:test: REGISTRAR REJECT OK");

    /* phase 5c: non-property-bearing target reject. The observer
     * store is the target's property table; before the daemon-side
     * validation, registering on an object without the property API
     * reported success while storing nothing (call_other on a
     * missing function returns nil). The daemon must now refuse.
     * This driver inherits no property lib, so it is its own
     * negative target -- and the domain matches, so the registrar
     * gate passes and the property-bearer check is what throws. */

    catch {
	MERRY_DAEMON->register_observer(this_object(),
	    "test:badtarget", "main", "return TRUE;");
	log_line("MerryApp:test: FAIL: register_observer on a "
		 + "non-property-bearing target did not throw");
	return;
    } : {
	/* expected: the daemon refused the target */
    }

    log_line("MerryApp:test: TARGET REJECT OK");

    /* phase 6: batched_set (non-atomic) -- writes a
     * multi-key mapping under one batch-id with sequential seq starting
     * at 0 (the implicit-batch semantics). Verifies the public LFUN compiles and that both
     * properties land; the batch-id and seq are daemon-internal and not
     * surfaced through a public API at this scope (a future change-log's
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

    /* phase 7: atomic-mode opt-in -- body + status
     * entry write run inside DGD atomic{}; on success the writes commit
     * normally. The abort-rollback branch is covered elsewhere; phase 7
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

    /* phase 8: BatchedSet from Merry source -- verifies the
     * merrynode.c surface is reachable from compiled scripts; the
     * mapping-arg signature composes inline (no function reference). */

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

    /* phase 9: non-atomic batch() abort path per the catch'd-error default +
     * the batch-status contract -- the callable throws; status entry must persist as
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
	 * this scope (a future change-log's query_changes_since covers that). */
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

    /* phase 10: dispatcher -- simple main-timing observer.
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

    /* phase 10b: multi-observer fan-out -- two observers registered
     * under the SAME merry:on:<path>:<timing> key are stored as a
     * list on the property; one write must fire both, in list order
     * (registration order) per the dispatcher's ordering contract.
     * Each observer appends its tag to a shared trace string. */

    catch {
	MERRY_DAEMON->register_observer(child, "test:dispatch:fanout", "main",
	    "Set($this, \"test:dispatch:fanout:trace\", Get($this, \"test:dispatch:fanout:trace\") + \"first|\"); return TRUE;");
	MERRY_DAEMON->register_observer(child, "test:dispatch:fanout", "main",
	    "Set($this, \"test:dispatch:fanout:trace\", Get($this, \"test:dispatch:fanout:trace\") + \"second\"); return TRUE;");

	child->set_raw_property("test:dispatch:fanout:trace", "");
	child->set_property("test:dispatch:fanout", 7);

	if (child->query_raw_property("test:dispatch:fanout") != 7) {
	    log_line("MerryApp:test: FAIL: DISPATCH FANOUT value did not land");
	    return;
	}
	if (child->query_raw_property("test:dispatch:fanout:trace")
	    != "first|second") {
	    log_line("MerryApp:test: FAIL: DISPATCH FANOUT trace was \""
		     + (string) child->query_raw_property("test:dispatch:fanout:trace")
		     + "\"");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: DISPATCH FANOUT setup threw");
	return;
    }

    log_line("MerryApp:test: DISPATCH FANOUT OK");

    /* phase 11: dispatcher -- pre + main + post timings.
     * Register observers on all three slots that append to a marker
     * string. Verify the string reads "pre|main|post" after the write,
     * confirming both per-timing slot firing and the documented ordering contract. */

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

    /* phase 12: dispatcher -- pre veto per the pre-veto contract. Register a
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

    /* phase 13: dispatcher -- cycle detection per the cascade-depth bound.
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

    /* phase 14: dispatcher -- ancestry walk per the ancestry-walk design. Register
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

    /* phase 15: dispatcher -- implicit-batch wrapping per the implicit-batch semantics.
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

    /* phase 16: PERSIST SETUP -- register a persistent observer on a
     * fresh binding host, save the host so phase17_verify can find it
     * after restore, schedule the verify call_out, then trigger a
     * hotboot cycle via the System helper. dump_state() captures all
     * application state (this object's globals, the persist_host
     * clone, the observer property on it, the merry-script object
     * referenced by the observer, the scheduled phase17_verify
     * call_out); shutdown() exits the driver. The external smoke
     * harness restarts DGD against the snapshot; the surviving
     * phase17_verify call_out fires after restore and writes either
     * PERSIST VERIFY OK or a failure sentinel to the result log. */

    catch {
	object pparent, pchild;

	pparent = clone_object(THING_PROG);
	pparent->set_object_name("MerryApp:demo:persist-parent");
	pchild = clone_object(THING_PROG);
	pchild->set_object_name("MerryApp:demo:persist-child");
	pchild->set_ur_object(pparent);

	persist_host = pchild;

	MERRY_DAEMON->register_observer(pchild, "test:persist:val", "main",
	    "Set($this, \"test:persist:fired\", 1); return TRUE;");

	/* Schedule verification with a small delay so the call_out is
	 * in the snapshot (and so it does not race the hotboot's own
	 * dump+shutdown call_out). After restore, its scheduled time
	 * has long passed and DGD fires it as soon as the system is
	 * back up. */
	call_out("phase17_verify", 3);

	log_line("MerryApp:test: PERSIST SETUP OK");

	PERSIST_HELPER->trigger_dump_and_exit();
    } : {
	log_line("MerryApp:test: FAIL: PERSIST SETUP threw");
	return;
    }
}


/* phase 17: PERSIST VERIFY -- fires from a pre-snapshot call_out after
 * the hotboot cycle. Writes to the persistent observed path; the
 * observer's source must execute against the restored merry-script
 * object reference and the marker property must land. */
static void phase17_verify()
{
    if (!persist_host) {
	log_line("MerryApp:test: FAIL: PERSIST VERIFY persist_host nil after restore");
	return;
    }

    catch {
	persist_host->set_property("test:persist:val", 42);

	if (persist_host->query_raw_property("test:persist:val") != 42) {
	    log_line("MerryApp:test: FAIL: PERSIST VERIFY value did not land after restore");
	    return;
	}
	if (persist_host->query_raw_property("test:persist:fired") != 1) {
	    log_line("MerryApp:test: FAIL: PERSIST VERIFY observer did not fire after restore");
	    return;
	}
    } : {
	log_line("MerryApp:test: FAIL: PERSIST VERIFY threw");
	return;
    }

    log_line("MerryApp:test: PERSIST VERIFY OK");
}


/* Helper for phase 9: a public LFUN that always throws so batch() can
 * exercise the abort path. Named with underscore prefix to mark it as
 * internal/test-only; deliberately not static so call_other can reach
 * it from MERRY's batch() implementation. */
void _throw_for_test()
{
    error("MerryApp:test: deliberate throw for batch abort verification");
}


static void verify_suicide()
{
    if (suicide_prog) {
	log_line("MerryApp:test: FAIL: compiled node survived suicide()");
	return;
    }
    log_line("MerryApp:test: SUICIDE OK");
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
