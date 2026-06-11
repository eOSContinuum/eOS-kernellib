/*
 * Domain initd for the minimal signal application.
 *
 * Compiles the property-bearing clonable and the boot-time test
 * driver. lib/app.c is pulled in by sys/test's inherit chain; libs
 * are lazy-loaded by DGD via their inheritors.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/thing");
    compile_object("sys/test");
}
