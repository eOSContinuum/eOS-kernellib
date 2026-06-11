/*
 * Domain initd for the reference Chat application.
 *
 * Compiles the Room + User clonables, the admin-token LWO, the chat
 * and admin daemons, and the boot-time test driver. lib/app.c is
 * pulled in by sys/test's inherit chain; libs are lazy-loaded by DGD
 * via their inheritors so no explicit compile_object is needed.
 *
 * sys/chat and sys/admin are compiled here so they exist as masters
 * once System/initd's domain-load loop returns. The test driver fires
 * its call_out after that point and calls into both daemons.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/room");
    compile_object("obj/user");
    compile_object("data/admin_token");
    compile_object("sys/chat");
    compile_object("sys/admin");
    compile_object("sys/test");
}
