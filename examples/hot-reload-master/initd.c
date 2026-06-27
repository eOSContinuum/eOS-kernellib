/*
 * Domain initd for the hot-reload-master regression.
 *
 * Compiles the clonable counter master and the boot-time test driver.
 * sys/test clones the counter, advances its state, recompiles the
 * counter master in place, and asserts the existing clone runs the new
 * program while keeping its state.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/counter");
    compile_object("sys/test");
}
