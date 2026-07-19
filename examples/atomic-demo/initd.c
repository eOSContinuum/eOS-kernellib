/*
 * Domain initd for the atomic-rollback demonstration.
 *
 * Compiles the counter master and the per-connection HTTP server at boot
 * so the first incoming HTTP connection can clone the server without
 * racing against compilation, and the counter responds to the first GET
 * /counter without an on-demand compile.
 *
 * sys/test is the headless boot-time regression that run-example.sh
 * drives; it performs the same caught-failure increment in-process and
 * asserts the rollback.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("counter");
    compile_object("obj/server");
    compile_object("sys/test");
}
