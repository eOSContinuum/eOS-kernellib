/*
 * Test-helper exposing a dump-and-exit path to /usr/ callers.
 *
 * dump_state() and shutdown() are gated to System-creator in
 * /kernel/lib/auto. Example applications that verify orthogonal-
 * persistence properties (observer survival, vault-state round-trips,
 * scheduled call_out resumption) need a way to write a snapshot and
 * exit cleanly so an external test harness can restart the driver
 * against the snapshot. This object exposes a single LFUN that any
 * /usr/ caller can invoke; the dump + shutdown sequence runs via
 * call_out so the caller's stack unwinds before the snapshot is taken
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
 * substrate's observer cache and batch state) survives the cycle via
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
