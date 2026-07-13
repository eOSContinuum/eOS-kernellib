/*
 * Domain initd for the reference HTTPS application.
 *
 * Compiles the per-connection TLS server master at boot so the first
 * incoming HTTPS connection can clone it without racing against
 * compilation.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/tls_server");
}
