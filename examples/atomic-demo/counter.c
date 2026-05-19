/*
 * Counter master for the atomic-rollback demonstration.
 *
 * Holds a private int and exposes two methods:
 *
 *   query()                   -- return the current counter value.
 *   increment_with_failure()  -- declared `atomic`. Mutates counter, then
 *                                error()s. The host runtime treats the body
 *                                as a single transactional unit and rolls
 *                                back the mutation when the error fires.
 *
 * The `atomic` modifier is what binds the mutation and the error into one
 * envelope. Without it, the mutation persists past the error in any dispatch
 * path that catches the error somewhere up the stack. With it, the host
 * restores counter to its pre-call value before the catch observes the error.
 *
 * The HTTP server in obj/server.c catches the error so the dispatch can
 * report it as a 200 response. The atomic rollback fires regardless of catch
 * -- catch determines whether the error propagates out of the dispatch path,
 * not whether the rollback fires.
 */

inherit "/usr/System/lib/auto";

private int counter;

static void create()
{
    ::create();
    counter = 0;
}

int query()
{
    return counter;
}

atomic void increment_with_failure()
{
    counter += 1;
    error("deliberate failure for atomic-rollback demonstration");
}
