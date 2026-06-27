/*
 * Boot-time test driver for the hot-reload-master regression.
 *
 * Demonstrates the load-bearing hot-reload guarantee: recompiling a
 * master in place propagates the new program to its EXISTING clones,
 * and each clone keeps its own dataspace (its variables' values) across
 * the recompile. Steps:
 *
 *   1. clone the counter, bump it three times -> count == 3, version
 *      "v1".                                              (CLONE STATE OK)
 *   2. recompile the counter MASTER with new source whose version()
 *      returns "v2"; the count variable and bump/query are unchanged.
 *   3. from the NEXT dispatch, the existing clone reports version "v2"
 *      -- its code followed the recompiled master.          (PROPAGATE OK)
 *   4. the same clone still reports count == 3 -- its dataspace survived
 *      the recompile.                                  (STATE SURVIVED OK)
 *
 * Steps 3/4 run from a fresh call_out, not inline: the new program takes
 * effect on the next dispatch, while the recompiling thread's in-flight
 * calls finish on the old program (docs/code-lifecycle.md). This covers
 * the clone-propagation half of the primitive; the library-inheritance
 * cascade (a recompiled parent lib re-touching its inheritors through
 * the upgrade daemon) is a separate, heavier mechanism not exercised
 * here.
 *
 * Pass/fail via /usr/Reload/data/test-result.log, the application-tier
 * convention shared by the bundled examples. Assertions target the
 * clone's reported values, not cosmetic text.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

# define COUNTER	"/usr/Reload/obj/counter"
# define RESULT_FILE	"/usr/Reload/data/test-result.log"
# define NEW_SOURCE	("inherit \"/usr/System/lib/auto\";\n" +		\
			 "int count;\n" +				\
			 "void bump() { count++; }\n" +			\
			 "int query() { return count; }\n" +		\
			 "string version() { return \"v2\"; }\n")

object clone;

private void log_line(string msg);
static void verify_after_reload();


static void create()
{
    /* Defer to a call_out so every domain initd has run and the
     * counter master is compiled before the driver clones it. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    mixed err;

    /* /usr/Reload/data/ may not exist on first boot. */
    catch(make_dir("/usr/Reload/data"));
    catch(remove_file(RESULT_FILE));

    log_line("Reload:test: starting");

    /* Step 1: clone and advance the per-clone state. */
    err = catch(clone = clone_object(COUNTER));
    if (err || !clone) {
	log_line("Reload:test: FAIL: could not clone the counter master");
	return;
    }
    clone->bump();
    clone->bump();
    clone->bump();
    if (clone->query() != 3 || clone->version() != "v1") {
	log_line("Reload:test: FAIL: clone setup (expected count 3, version v1)");
	return;
    }
    log_line("Reload:test: CLONE STATE OK");

    /* Step 2: recompile the master's program in place. The kernel's
     * compile_object wrapper returns nil for an /obj/ (clonable) path
     * even on success, so only a thrown error signals failure here; the
     * propagation check below is the real confirmation the recompile
     * took. */
    err = catch(compile_object(COUNTER, NEW_SOURCE));
    if (err) {
	log_line("Reload:test: FAIL: recompile of the counter master threw: " + err);
	return;
    }

    /* Steps 3/4 verify from the next dispatch, not inline. */
    call_out("verify_after_reload", 0);
}

static void verify_after_reload()
{
    string ver;
    int n;
    mixed err;

    /* Step 3: the existing clone runs the recompiled program. */
    err = catch(ver = clone->version());
    if (err || ver != "v2") {
	log_line("Reload:test: FAIL: clone did not pick up the recompiled program");
	return;
    }
    log_line("Reload:test: PROPAGATE OK");

    /* Step 4: the existing clone kept its dataspace. */
    err = catch(n = clone->query());
    if (err || n != 3) {
	log_line("Reload:test: FAIL: clone dataspace did not survive the recompile");
	return;
    }
    log_line("Reload:test: STATE SURVIVED OK");
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
