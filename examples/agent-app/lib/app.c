/*
 * Agent-application library.
 *
 * Thin marker layer so the domain's singletons under sys/ share an
 * inherit chain; mirrors the shape of examples/webauthn-app/lib/app.c.
 *
 * Inheritors are static masters (one-of-a-kind daemons under sys/),
 * not clonables.
 */

static void create()
{
}
