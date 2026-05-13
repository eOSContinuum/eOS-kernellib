/* SPDX-License-Identifier: BSD-2-Clause-Patent */

/*
 * Domain initd for the reference HTTP application.
 *
 * Compiles the per-connection server master at boot so the first incoming
 * HTTP connection can clone it without racing against compilation.
 */

# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/server");
}
