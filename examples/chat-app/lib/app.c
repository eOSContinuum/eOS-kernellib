/*
 * Chat-application library.
 *
 * Thin marker layer that lets the demonstration's per-domain
 * singletons (test driver, chat daemon, admin daemon) share an
 * inherit chain. Mirrors the shape of examples/vault-app/lib/app.c
 * and examples/merry-app/lib/app.c so application authors see a
 * parallel pattern across the bundled examples.
 *
 * Inheritors are static masters (sys/chat, sys/admin, sys/test),
 * not clonables.
 */

static void create()
{
}
