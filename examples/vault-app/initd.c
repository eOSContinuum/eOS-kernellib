/*
 * Domain initd for the reference Vault application.
 *
 * Compiles the property-bearing clonable and the boot-time test driver.
 * lib/app.c is pulled in automatically by sys/test's inherit chain;
 * there is no explicit compile_object("lib/app") here because libs are
 * lazy-loaded by DGD via their inheritors.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/thing");
    compile_object("sys/test");
}
