/*
 * portd: the port-label registry.
 *
 * The label layer over the kernel's numeric port-to-manager registration
 * (set_telnet_manager and friends on /kernel/sys/userd): System-tier boot
 * code declares a label ("admin", "http") for a configured (type, index)
 * port slot, and connection managers register by label, so a manager
 * names its port by role rather than by position in the .dgd port list.
 * The kernel surface is unchanged underneath: register_manager forwards
 * to the kernel userd, which keeps its own System-tier gate and
 * first-registration-wins semantics.
 *
 * Where the kernel registration is silent about mistakes (a port with no
 * registered manager falls back to the default user object), this layer
 * refuses loudly: declaring a label for an unconfigured port slot,
 * redeclaring a label, or registering against an undeclared label are
 * all errors. Declaration and registration are System-gated; the query
 * surface is open to every tier. A registration made directly against
 * the kernel userd bypasses this daemon and is invisible to it; the
 * manager a query reports is the one registered through the label path.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <status.h>
# include <portd.h>

# define SYSTEMAUTO	"/usr/System/lib/auto"

inherit auto AUTO;
inherit SYSTEMAUTO;

private mapping labels;		/* label -> ({ type, index }) */
private mapping managers;	/* label -> manager registered by label */

/*
 * NAME:	ports_for()
 * DESCRIPTION:	return the configured port numbers for a connection type
 */
private int *ports_for(string type)
{
    switch (type) {
    case "telnet":
	return status(ST_TELNETPORTS);

    case "binary":
	return status(ST_BINARYPORTS);

    case "datagram":
	return status(ST_DATAGRAMPORTS);

    default:
	error("Unknown connection type: " + type);
    }
}

/*
 * NAME:	declare()
 * DESCRIPTION:	validate and record a label declaration
 */
private void declare(string label, string type, int index)
{
    int *ports;

    if (!label || label == "") {
	error("Bad label");
    }
    ports = ports_for(type);
    if (index < 0 || !ports || index >= sizeof(ports)) {
	error("No configured " + type + " port at index " + index);
    }
    if (labels[label]) {
	error("Label already declared: " + label);
    }
    labels[label] = ({ type, index });
}

/*
 * NAME:	create()
 * DESCRIPTION:	declare the canonical labels for the shipped port slots
 */
static void create()
{
    int *ports;

    labels = ([ ]);
    managers = ([ ]);
    ports = status(ST_TELNETPORTS);
    if (ports && sizeof(ports) != 0) {
	declare("admin", "telnet", 0);
    }
    ports = status(ST_BINARYPORTS);
    if (ports && sizeof(ports) != 0) {
	declare("http", "binary", 0);
	if (sizeof(ports) > 1) {
	    declare("https", "binary", 1);
	}
    }
}

/*
 * NAME:	declare_label()
 * DESCRIPTION:	declare a label for a configured port slot (System only)
 */
void declare_label(string label, string type, int index)
{
    if (!SYSTEM()) {
	error("Access denied");
    }
    declare(label, type, index);
}

/*
 * NAME:	register_manager()
 * DESCRIPTION:	register a connection manager by label (System only)
 */
void register_manager(string label, object manager)
{
    mixed *slot;

    if (!SYSTEM()) {
	error("Access denied");
    }
    if (!manager) {
	error("Bad manager");
    }
    slot = labels[label];
    if (!slot) {
	error("No such label: " + label);
    }
    if (managers[label]) {
	error("Manager already registered for label: " + label);
    }
    call_other(USERD, "set_" + slot[0] + "_manager", slot[1], manager);
    managers[label] = manager;
}

/*
 * NAME:	query_label()
 * DESCRIPTION:	resolve a label to ({ type, index, port, manager }), or
 *		nil for an undeclared label
 */
mixed *query_label(string label)
{
    mixed *slot;

    slot = labels[label];
    if (!slot) {
	return nil;
    }
    return ({ slot[0], slot[1], ports_for(slot[0])[slot[1]], managers[label] });
}

/*
 * NAME:	query_labels()
 * DESCRIPTION:	return the declared labels
 */
string *query_labels()
{
    return map_indices(labels);
}
