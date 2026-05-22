/*
 * Domain initd for the reference Merry application.
 *
 * Compiles the property + ur-bearing demonstration clonable and the
 * boot-time test driver. lib/app.c is pulled in by sys/test's inherit
 * chain; no explicit compile_object("lib/app") is needed because libs
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
