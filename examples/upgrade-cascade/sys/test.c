/*
 * Boot-time test driver for the upgrade-cascade regression.
 *
 * Demonstrates the library-inheritance half of the hot-reload primitive:
 * upgrading a parent LIBRARY through the upgrade daemon
 * (/usr/System/sys/upgraded) recompiles its inheritors and
 * call_touch-patches their existing clones, while each clone keeps its
 * own dataspace. This is the mechanism hot-reload-master leaves
 * unexercised (that example recompiles a single master in place). Steps:
 *
 *   1. clone two widgets, advance them to different counts (3 and 5),
 *      confirm both compose with the v1 library.       (CLONE SETUP OK)
 *   2. stage new library source on disk: shape_version() "v2", scale()
 *      multiplies by 3. A source-file write alone changes nothing live --
 *      both clones still run v1.                 (STAGED SOURCE INERT OK)
 *   3. drive the upgrade daemon via the System auto library's upgrade()
 *      wrapper, atomically, with this driver as the patchtool.
 *                                                   (UPGRADE ACCEPTED OK)
 *   4. from a later dispatch, the existing clones report the v2 library
 *      through their unchanged inheritor code,     (CASCADE PROPAGATE OK)
 *      with the v2 behavior,                           (LIB BEHAVIOR OK)
 *      their counts intact,                     (CLONE STATE SURVIVED OK)
 *      and patch() having run exactly once per clone through the
 *      call_touch gate.                                 (CLONE PATCH OK)
 *
 * Step 4 runs from a delayed call_out: the daemon patches touched objects
 * through a chain of zero-delay callouts (one object per task), and the
 * delay lets that chain finish so the assertions see the settled state.
 *
 * Pass/fail via /usr/Cascade/data/test-result.log, the application-tier
 * convention shared by the bundled examples. Assertions target the
 * clones' reported values, not cosmetic text.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

# define WIDGET		"/usr/Cascade/obj/widget"
# define LIB_SOURCE	"/usr/Cascade/lib/shape.c"
# define UPGRADED	"/usr/System/sys/upgraded"
# define RESULT_FILE	"/usr/Cascade/data/test-result.log"
# define NEW_LIB_SOURCE	("inherit \"/usr/System/lib/auto\";\n" +	\
			 "string shape_version() { return \"v2\"; }\n" + \
			 "int scale(int n) { return n * 3; }\n")

object clone1, clone2;

private void log_line(string msg);
static void verify_after_upgrade();


static void create()
{
    /* Defer to a call_out so every domain initd has run and the widget
     * master is compiled before the driver clones it. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    mixed err, result;
    int i;

    /* /usr/Cascade/data/ may not exist on first boot. */
    catch(make_dir("/usr/Cascade/data"));
    catch(remove_file(RESULT_FILE));

    log_line("Cascade:test: starting");

    /* Step 1: two clones with distinct per-clone state. */
    err = catch(clone1 = clone_object(WIDGET));
    if (err || !clone1) {
	log_line("Cascade:test: FAIL: could not clone the widget master");
	return;
    }
    clone2 = clone_object(WIDGET);
    for (i = 0; i < 3; i++) {
	clone1->bump();
    }
    for (i = 0; i < 5; i++) {
	clone2->bump();
    }
    if (clone1->query() != 3 || clone2->query() != 5 ||
	clone1->describe() != "shape v1" || clone2->describe() != "shape v1" ||
	clone1->scale(2) != 4) {
	log_line("Cascade:test: FAIL: clone setup (expected counts 3/5, " +
		 "\"shape v1\", scale(2) == 4)");
	return;
    }
    /* Index logical names: the console cycle
     * (scripts/verbsets/operator-upgrade.verbset) addresses these exact
     * clones by name to assert its own upgrade landed on them. */
    clone1->set_object_name("Cascade:demo:widget1");
    clone2->set_object_name("Cascade:demo:widget2");
    log_line("Cascade:test: CLONE SETUP OK");

    /* Step 2: stage the v2 library source. The file write alone must not
     * change the live objects. */
    catch(remove_file(LIB_SOURCE));
    err = catch(write_file(LIB_SOURCE, NEW_LIB_SOURCE));
    if (err) {
	log_line("Cascade:test: FAIL: could not stage the new library " +
		 "source: " + err);
	return;
    }
    if (clone1->describe() != "shape v1" || clone1->scale(2) != 4) {
	log_line("Cascade:test: FAIL: a source-file write changed live " +
		 "objects before any upgrade");
	return;
    }
    log_line("Cascade:test: STAGED SOURCE INERT OK");

    /* Step 3: drive the upgrade daemon -- atomically, with this driver as
     * the patchtool so existing clones are call_touch-patched. A string
     * result is a refusal; a non-empty array lists failed compiles. */
    err = catch(result = upgrade(({ LIB_SOURCE }), TRUE, this_object()));
    if (err) {
	log_line("Cascade:test: FAIL: upgrade threw: " + err);
	return;
    }
    if (typeof(result) != T_ARRAY || sizeof(result) != 0) {
	log_line("Cascade:test: FAIL: upgrade was not accepted cleanly");
	return;
    }
    log_line("Cascade:test: UPGRADE ACCEPTED OK");

    /* Step 4 verifies after the daemon's patch callouts have run. */
    call_out("verify_after_upgrade", 1);
}

/*
 * The upgrade daemon visits each touched object through this hook (one
 * zero-delay callout per object). Any call into the object fires the
 * pending touch first, which reaches the object's patch() function.
 */
void do_patch(object obj)
{
    if (previous_program() == UPGRADED) {
	obj->query();
    }
}

static void verify_after_upgrade()
{
    string d1, d2;
    mixed err;

    /* The existing clones' inheritor code follows the upgraded library. */
    err = catch(d1 = clone1->describe());
    if (!err) {
	err = catch(d2 = clone2->describe());
    }
    if (err || d1 != "shape v2" || d2 != "shape v2") {
	log_line("Cascade:test: FAIL: existing clones did not pick up the " +
		 "upgraded library");
	return;
    }
    log_line("Cascade:test: CASCADE PROPAGATE OK");

    /* And its behavior, not just its version string. */
    if (clone1->scale(2) != 6 || clone2->scale(2) != 6) {
	log_line("Cascade:test: FAIL: upgraded library behavior did not " +
		 "reach existing clones");
	return;
    }
    log_line("Cascade:test: LIB BEHAVIOR OK");

    /* Per-clone dataspace survived the inheritor recompile. */
    if (clone1->query() != 3 || clone2->query() != 5) {
	log_line("Cascade:test: FAIL: clone dataspace did not survive the " +
		 "upgrade (expected counts 3/5)");
	return;
    }
    log_line("Cascade:test: CLONE STATE SURVIVED OK");

    /* patch() ran exactly once per clone through the call_touch gate. */
    if (clone1->query_patched() != 1 || clone2->query_patched() != 1) {
	log_line("Cascade:test: FAIL: patch() did not run exactly once per " +
		 "clone (got " + clone1->query_patched() + " and " +
		 clone2->query_patched() + ")");
	return;
    }
    log_line("Cascade:test: CLONE PATCH OK");
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
