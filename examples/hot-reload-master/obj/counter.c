/*
 * Clonable counter master for the hot-reload-master regression.
 *
 * Carries per-clone state in a plain variable (count) and reports a
 * version string. The regression clones it, advances the count, then
 * recompiles THIS master in place: the existing clone must run the new
 * program (a clone's code follows its recompiled master) while keeping
 * its count (the clone's dataspace survives the recompile). That pair is
 * the load-bearing hot-reload guarantee -- see docs/code-lifecycle.md
 * and docs/runtime-primitives.md section 4.
 *
 * Clonable objects live under obj/ (the kernel's clone_object only
 * accepts paths containing /obj/). sys/test recompiles this master with
 * new source whose version() returns "v2"; the count variable and
 * bump/query stay identical so the dataspace layout is unchanged across
 * the recompile.
 */

inherit "/usr/System/lib/auto";

int count;

void bump()
{
    count++;
}

int query()
{
    return count;
}

string version()
{
    return "v1";
}
