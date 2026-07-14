/*
 * Domain initd for the WebAuthn substrate example.
 *
 * Compiles the boot-time test driver; lib/app.c is pulled in by the
 * driver's inherit chain (libs are lazy-loaded by DGD via their
 * inheritors).
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("sys/test");
}
