/*
 * Domain initd for the hot-reload demonstration.
 *
 * Compiles the greeting master and the per-connection HTTP server at boot
 * so the first incoming HTTP connection can clone the server without
 * racing against compilation, and the greeting responds to the first GET
 * /greet without an on-demand compile.
 *
 * Subsequent recompiles of the greeting (via POST /compile, or in-process
 * from sys/test) replace the master's program in place; no further work
 * happens here.
 *
 * sys/test is the headless boot-time regression that run-example.sh drives;
 * it reloads the greeting in-process and asserts the new program is live.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("greeting");
    compile_object("obj/server");
    compile_object("sys/test");
}
