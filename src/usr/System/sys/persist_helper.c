/*
 * The /usr/-facing persistence surface: programmatic access to the
 * kernel layer's snapshot operations. dump_state() and shutdown() are
 * gated to System-creator in /kernel/lib/auto, so /usr/ callers reach
 * them through the two entry points here, each with its own posture.
 *
 * trigger_dump() -- the durability primitive. Writes a snapshot while
 * the runtime keeps serving, for applications that need snapshot
 * cadence: periodic dumps, or dumps at workload milestones. Gated by
 * the "persist.snapshot" capability against the calling object's
 * owner: default-deny, operator-granted and -revocable per principal
 * (`capability grant persist.snapshot <principal>` on the admin
 * console). The dump_state() kfun only marks the state for dumping --
 * the driver writes the snapshot after the current task completes --
 * so the call returns immediately, the caller's stack has unwound by
 * write time, and no call_out indirection is needed. Cadence itself
 * (intervals, milestone triggers) stays application-side by design;
 * an armed call_out loop rides each snapshot across restores, so a
 * caller's cadence survives a restore with no re-arming.
 *
 * trigger_dump_and_exit() -- the test-harness cycle path. Example
 * applications that verify orthogonal-persistence properties (observer
 * survival, vault-state round-trips, scheduled call_out resumption)
 * need a way to write a snapshot and exit cleanly so an external test
 * harness can restart the driver against the snapshot. Any /usr/
 * caller can invoke it; the dump + shutdown sequence runs via call_out
 * so the caller's stack unwinds before the snapshot is taken
 * (otherwise the snapshot captures the caller mid-execution and the
 * resumed run is messy).
 *
 * DGD's in-process hotboot (dump_state(TRUE) + shutdown(TRUE)) re-execs
 * the driver against both the primary and secondary snapshot files
 * named in the .dgd config's hotboot list; on a cold-boot first cycle
 * the secondary doesn't exist yet and re-exec errors. The dump-and-
 * exit shape lets the external harness restart DGD with just the
 * primary snapshot, sidestepping the rotation requirement, at the cost
 * of one process-restart in the harness.
 *
 * Application state (objects, properties, call_outs, the dispatcher
 * substrate's observer cache and batch state) survives either cycle via
 * DGD's standard orthogonal-persistence guarantees.
 */

# include <kernel/kernel.h>
# include <kernel/capability.h>

inherit "/usr/System/lib/auto";
inherit "/kernel/lib/capability";

static void create()
{
    ::create();
}

/*
 * The capability check runs at this public entry, against the true
 * external caller (previous_object() here is the calling surface, not
 * a helper frame). The principal is the caller's owner -- a /usr
 * domain, or a console operator's user name for code-verb calls.
 */
void trigger_dump()
{
    require_member("persist.snapshot",
		   previous_object() ? previous_object()->query_owner() : nil);
    dump_state(FALSE);
}

void trigger_dump_and_exit()
{
    call_out("_do_dump_and_exit", 0);
}

static void _do_dump_and_exit()
{
    /*
     * dump_state/shutdown are gated to System-creator objects in
     * /kernel/lib/auto (which holds the private `creator`). Surface that
     * fixed-principal check here in the uniform capability posture,
     * adjacent to the privileged calls, via the accessible owner; auto
     * remains the foundational backstop.
     */
    require(query_owner() == "System", "Permission denied");
    dump_state(FALSE);
    shutdown();
}
