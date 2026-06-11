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
    /* sys/config is deliberately NOT compiled here: the test driver's
     * singleton respawn assertion needs the program unloaded so the
     * Vault's find-or-load branch compiles it fresh. */
}
