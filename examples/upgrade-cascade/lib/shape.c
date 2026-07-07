/*
 * Parent library for the upgrade-cascade regression.
 *
 * The inheritable code under test: obj/widget inherits this library, and
 * the regression upgrades THIS source file through the upgrade daemon
 * (/usr/System/sys/upgraded) -- the daemon destructs the old library
 * issue, recompiles every inheritor against the new code, and (with a
 * patchtool) call_touch-patches the inheritors' existing clones. See
 * docs/runtime-primitives.md section 4 and docs/code-lifecycle.md.
 *
 * sys/test stages replacement source whose shape_version() returns "v2"
 * and whose scale() multiplies by 3, then drives the upgrade; existing
 * widget clones must report the new version AND the new behavior from
 * their next dispatch, without losing per-clone state.
 */

inherit "/usr/System/lib/auto";

string shape_version()
{
    return "v1";
}

int scale(int n)
{
    return n * 2;
}
