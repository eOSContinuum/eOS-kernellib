/*
 * Merry-application library.
 *
 * Thin marker layer that lets the demonstration's per-domain singletons
 * (the test driver under sys/) share an inherit chain even though there
 * is no daemon-side state to factor here yet. Mirrors the shape of
 * examples/vault-app/lib/app.c so application authors see a parallel
 * pattern across the bundled examples.
 *
 * Inheritors are static masters (one-of-a-kind daemons under sys/),
 * not clonables.
 */

static void create()
{
}
