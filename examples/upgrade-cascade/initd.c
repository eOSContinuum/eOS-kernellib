/*
 * Domain initd for the upgrade-cascade regression.
 *
 * Compiles the clonable widget master (which pulls in lib/shape as its
 * parent library) and the boot-time test driver. sys/test clones the
 * widget, stages new library source, drives the upgrade daemon, and
 * asserts the cascade: inheritors recompiled, existing clones on the new
 * program with their state intact, patch() run once per clone.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/widget");
    compile_object("sys/test");
}
