/*
 * Boot-time test driver for the minimal signal application.
 *
 * The whole demonstration is four steps:
 *
 *   1. clone the property host
 *   2. register a Merry observer for one property path
 *   3. write the property
 *   4. observe that the reaction already ran when the write returned
 *
 * The reaction is a one-line Merry script that sets a marker property
 * on the host. Because the dispatcher fires observers synchronously
 * inside the write, the marker is visible the moment set_property
 * returns -- no polling, no queue, no callback registration beyond
 * the one observer binding.
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/SignalApp/data/test-result.log (the application-tier logging
 * convention shared by the bundled examples).
 */

# include <type.h>
# include <kernel/kernel.h>

inherit "/usr/SignalApp/lib/app";

# define MERRY_DAEMON	"/usr/Merry/sys/merry"
# define THING_PROG	"/usr/SignalApp/obj/thing"
# define RESULT_FILE	"/usr/SignalApp/data/test-result.log"

static void run_tests();
private void log_line(string msg);


static void create()
{
    /* Defer to a call_out so every domain initd (including Merry's)
     * has run before the driver calls into the Merry daemon. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    /* /usr/SignalApp/data/ may not exist on first boot. */
    catch(make_dir("/usr/SignalApp/data"));
    catch(remove_file(RESULT_FILE));
    run_tests();
}


static void run_tests()
{
    object thing;

    log_line("SignalApp:test: starting");

    thing = clone_object(THING_PROG);

    catch {
	MERRY_DAEMON->register_observer(thing, "signal:watched", "main",
	    "Set($this, \"signal:fired\", 1); return TRUE;");

	thing->set_property("signal:watched", 42);

	if (thing->query_raw_property("signal:watched") != 42) {
	    log_line("SignalApp:test: FAIL: the written value did not land");
	    return;
	}
	if (thing->query_raw_property("signal:fired") != 1) {
	    log_line("SignalApp:test: FAIL: the reaction did not fire");
	    return;
	}
    } : {
	log_line("SignalApp:test: FAIL: setup threw");
	return;
    }

    log_line("SignalApp:test: SIGNAL OK");
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
