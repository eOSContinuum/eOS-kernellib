/*
 * Inventory domain initd for the composite example.
 *
 * Compiles the domain's daemon, handler, loopback client, and test
 * driver at boot, then registers the domain's route prefixes with the
 * WWW route registry via the call_out-0 idiom: the System initd
 * iterates domain initds alphabetically (Inventory before WWW), so the
 * registry does not exist yet while this create() runs; a 0-second
 * call_out lands after the boot iteration commits
 * (docs/http-applications.md Cross-domain initialization order).
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

# define ROUTER		"/usr/WWW/sys/router"

static void create()
{
    ::create();
    compile_object("sys/inventoryd");
    compile_object("sys/handler");
    compile_object("obj/client");
    compile_object("sys/demo");
    compile_object("sys/test");
    call_out("register_routes", 0);
}

static void register_routes()
{
    ROUTER->register("/inventory", "/usr/Inventory/sys/handler");
    ROUTER->register("/auth", "/usr/Inventory/sys/handler");
    ROUTER->register("/demo", "/usr/Inventory/sys/demo");
}
