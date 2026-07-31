/*
 * Domain initd for the console-ext reference application.
 *
 * Compiles the one daemon. Its create() attempts the boot-time verb
 * registration; when the domain has not yet been approved for
 * admin_console.extend, the attempt is refused and the daemon records
 * the refusal for later inspection (see sys/extd.c).
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("sys/extd");
}
