/*
 * Clonable inheritor for the upgrade-cascade regression.
 *
 * Inherits the shape library and composes with it (describe() calls the
 * library's shape_version()). This source file never changes during the
 * regression: when sys/test upgrades lib/shape.c through the upgrade
 * daemon, the daemon recompiles THIS master as a dependent, and existing
 * clones follow the recompiled program while keeping their per-clone
 * count.
 *
 * patch() is the call_touch hook: the upgrade daemon marks each existing
 * clone with call_touch and then visits it, and the System auto library's
 * _F_touch gate calls patch() on the first reference after the upgrade.
 * The patched counter proves the hook ran exactly once per clone -- the
 * place a real application would migrate clone state to a new layout.
 *
 * Clonable objects live under obj/ (the kernel's clone_object only
 * accepts paths containing /obj/).
 */

inherit "/usr/Cascade/lib/shape";

int count;		/* per-clone state; must survive the upgrade */
int patched;		/* times patch() ran; 1 after the upgrade */

void bump()
{
    count++;
}

int query()
{
    return count;
}

int query_patched()
{
    return patched;
}

string describe()
{
    return "shape " + shape_version();
}

void patch()
{
    if (previous_program() == "/usr/System/lib/auto") {
	patched++;
    }
}
