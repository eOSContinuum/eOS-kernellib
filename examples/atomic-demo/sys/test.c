/*
 * Boot-time test driver for the atomic-rollback demonstration.
 *
 * The HTTP smoke (smoke.sh) drives the rollback over the wire; this
 * driver does the same demonstration in-process so run-example.sh can
 * verify it headless, with no HTTP client. Three assertions:
 *
 *   1. query() returns 0 at cold boot -- the pre-call baseline.
 *   2. catch(increment_with_failure()) observes the deliberate error,
 *      and the caught message carries the counter's own failure text --
 *      proof the atomic body actually ran to its error() (the increment
 *      executed), so the next assertion is not vacuous.
 *   3. query() still returns 0 after the caught call -- the runtime
 *      rolled the increment back. catch determines whether the error
 *      propagates, not whether the rollback fires (counter.c's own
 *      header records that contract).
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/WWW/data/test-result.log (the application-tier logging
 * convention shared by the bundled examples).
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

# define COUNTER	"/usr/WWW/counter"
# define RESULT_FILE	"/usr/WWW/data/test-result.log"
# define FAILURE_TEXT	"deliberate failure for atomic-rollback demonstration"

private void log_line(string msg);


static void create()
{
    /* Defer to a call_out so every domain initd has run and the
     * counter master is compiled before the driver calls it. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    int before, after;
    string err;

    /* /usr/WWW/data/ may not exist on first boot. */
    catch(make_dir("/usr/WWW/data"));
    catch(remove_file(RESULT_FILE));

    log_line("Atomic:test: starting");

    before = COUNTER->query();
    if (before != 0) {
	log_line("Atomic:test: FAIL: cold-boot counter is " + before +
		 ", not 0");
	return;
    }
    log_line("Atomic:test: INITIAL OK");

    err = catch(COUNTER->increment_with_failure());
    if (!err || sscanf(err, "%*s" + FAILURE_TEXT) == 0) {
	log_line("Atomic:test: FAIL: expected the deliberate failure, " +
		 "caught: " + (err ? err : "(nothing)"));
	return;
    }
    log_line("Atomic:test: BODY-RAN OK");

    after = COUNTER->query();
    if (after != 0) {
	log_line("Atomic:test: FAIL: counter is " + after +
		 " after the caught atomic error; mutation escaped");
	return;
    }
    log_line("Atomic:test: ROLLBACK OK");
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
