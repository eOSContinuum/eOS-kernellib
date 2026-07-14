/*
 * WWW domain initd for the composite example.
 *
 * Compiles the route registry and both mount-point servers at boot so
 * the first incoming connection clones without racing compilation.
 * Other domains register their handlers with sys/router from their own
 * initds via the call_out-0 idiom (docs/http-applications.md
 * Cross-domain initialization order); this initd does not know or care
 * which applications exist.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("sys/router");
    compile_object("obj/server");
    compile_object("obj/tls_server");
}
