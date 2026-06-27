/*
 * Boot-time test driver for the hot-reload demonstration.
 *
 * The HTTP smoke (smoke.sh) drives the recompile over the wire; this
 * driver does the same reload in-process so run-example.sh can verify
 * it headless, with no HTTP client. The demonstration is three steps:
 *
 *   1. call greeting->greet() at cold boot and confirm the original
 *      program's return value.
 *   2. compile_object the greeting master with new source returning a
 *      distinct value -- the program is replaced in place.
 *   3. from the NEXT dispatch, call greeting->greet() again and confirm
 *      the new value is live.
 *
 * Step 3 runs from a fresh call_out, not inline after the recompile, on
 * purpose: the new program takes effect for the next dispatch, while an
 * in-flight call (the recompiling thread itself) finishes on the old
 * program (docs/runtime-primitives.md section 4; the same guarantee the
 * HTTP server's dispatch() comment records). Verifying inline would read
 * the old program and is the wrong assertion about the primitive.
 *
 * compile_object replacing a master's program in place, with the next
 * dispatch picking up the new program and no DGD restart, is the
 * hot-reload primitive.
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/WWW/data/test-result.log (the application-tier logging
 * convention shared by the bundled examples). Assertions target the
 * program's returned value, not any cosmetic text.
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

# define GREETING	"/usr/WWW/greeting"
# define RESULT_FILE	"/usr/WWW/data/test-result.log"
# define BEFORE_TEXT	"hello before recompile\n"
# define AFTER_TEXT	"hello after recompile\n"
# define NEW_SOURCE	"string greet() { return \"hello after recompile\\n\"; }\n"

private void log_line(string msg);
static void reload_phase1();
static void reload_phase2();


static void create()
{
    /* Defer to a call_out so every domain initd has run and the
     * greeting master is compiled before the driver calls it. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    /* /usr/WWW/data/ may not exist on first boot. */
    catch(make_dir("/usr/WWW/data"));
    catch(remove_file(RESULT_FILE));
    reload_phase1();
}


static void reload_phase1()
{
    string before;
    object obj;
    mixed err;

    log_line("HotReload:test: starting");

    err = catch(before = GREETING->greet());
    if (err || before != BEFORE_TEXT) {
	log_line("HotReload:test: FAIL: cold greet did not return the original program's value");
	return;
    }
    log_line("HotReload:test: INITIAL OK");

    /* Replace the greeting master's program in place. */
    err = catch(obj = compile_object(GREETING, NEW_SOURCE));
    if (err || !obj) {
	log_line("HotReload:test: FAIL: recompile of the greeting master failed");
	return;
    }

    /* Verify from the next dispatch (a fresh thread), not inline. */
    call_out("reload_phase2", 0);
}

static void reload_phase2()
{
    string after;
    mixed err;

    err = catch(after = GREETING->greet());
    if (err || after != AFTER_TEXT) {
	log_line("HotReload:test: FAIL: next dispatch did not pick up the new program");
	return;
    }
    log_line("HotReload:test: RELOAD OK");
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
