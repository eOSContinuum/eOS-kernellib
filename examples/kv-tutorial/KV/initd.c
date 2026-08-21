# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("sys/kv_daemon");
    compile_object("sys/kv_vault");
    compile_object("obj/entry");
}
