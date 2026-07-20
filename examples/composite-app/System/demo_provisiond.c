/*
 * Demo-only System-tier provisioner: the browser demo's stand-in for
 * the human operator.
 *
 * Delegation needs two operator actions before Delegate can succeed
 * from the browser: the controller must HOLD a capability (the console
 * verb `identity grant <uuid> <capability>`) and the capability must
 * be operator-flagged delegable (`capability delegable <capability>
 * on`). Neither is reachable from an application domain -- identityd's
 * grant path is System-gated and capabilityd's delegable flag is
 * KERNEL()-gated -- which is the platform's authorization model doing
 * its job: an application does not mint its own authority.
 *
 * For an interactive demo the operator's half is pre-provisioned: the
 * demo handler announces each fresh registration here, and this object
 * grants the demo capability (example:delegation-demo) to the verified
 * HUMAN record, standing exactly where a human operator would type
 * `identity grant <uuid> example:delegation-demo`. The delegable flag
 * stays a real console verb in the bring-up recipe: `capability
 * delegable example:delegation-demo on`.
 *
 * Demo-only, by construction and by deployment: this file lives in the
 * example tree, is copied to /usr/System/sys/ and console-compiled
 * only by the browser-demo bring-up (see the example README's
 * browser-path section), is part of no headless profile, and grants
 * exactly one hardcoded demo-namespace capability to verified human
 * identity records announced by the demo's own domain. Remove the
 * deployed copy at teardown; do not deploy outside a demo.
 */

# include <kernel/kernel.h>
# include <identityd.h>

inherit "/usr/System/lib/auto";

# define CAPABILITYD		"/kernel/sys/capabilityd"
# define DEMO_CAPABILITY	"example:delegation-demo"

/*
 * the demo handler announces a freshly registered identity; grant the
 * demo capability when the record is a live human identity that does
 * not already hold it. Idempotent, and validated on this side of the
 * tier boundary -- the announcement only names a uuid, the record's
 * kind decides.
 */
void welcome(string uuid)
{
    object identity;

    if (sscanf(previous_program(), "/usr/Inventory/%*s") == 0) {
	error("Access denied");
    }
    if (!uuid) {
	return;
    }
    identity = IDENTITYD->find_identity(uuid);
    if (!identity || identity->query_kind() != ID_KIND_HUMAN) {
	return;
    }
    if (!CAPABILITYD->is_allowed(DEMO_CAPABILITY,
				 identity->query_principal())) {
	IDENTITYD->grant_capability(uuid, DEMO_CAPABILITY);
    }
}
