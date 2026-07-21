/*
 * Inventory daemon: the composite example's persistent, observer-bearing,
 * capability-gated domain core.
 *
 * Three platform primitives meet here:
 *
 *   - Persistence: the items mapping is ordinary object state; it (and
 *     the observer binding below) survive snapshot restore with no
 *     serialization code.
 *   - Signals: the daemon is a property host (one inherit,
 *     /lib/util/properties). Every mutation writes the
 *     "inventory:last-event" property inside the same atomic function
 *     as the mutation; a Merry observer registered on that property
 *     appends the event to the "inventory:audit-log" property
 *     synchronously, inside the write. Mutation and audit therefore
 *     commit or roll back together.
 *   - Capability gate: wipe() is the platform-capability-gated admin
 *     LFUN. It asks capabilityd's public is_allowed choke-point whether
 *     the caller-proven principal holds ADMIN_CAPABILITY; the grant
 *     itself is operator work ("identity grant <uuid> ..." on the
 *     console) and cannot be performed from this tier. report() gates
 *     a read on REPORT_CAPABILITY through the same choke-point; the
 *     demo pre-provisions that one delegable, so it is the surface
 *     where a delegation's effect is directly observable.
 *
 * Application-tier authorization (who may edit an item) is decided
 * here too, but against plain application state (the item's creator),
 * not the capability store -- the three-layer split of
 * docs/identity.md.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/lib/util/properties";

# define MERRY		"/usr/Merry/sys/merry"
# define CAPABILITYD	"/kernel/sys/capabilityd"

# define ADMIN_CAPABILITY	"example:inventory-admin"
# define REPORT_CAPABILITY	"example:delegation-demo"
# define EVENT_PROP		"inventory:last-event"
# define AUDIT_PROP		"inventory:audit-log"

private mapping items;		/* id : ([ name, qty, creator ]) */
private int nextId;

static void create()
{
    ::create();
    items = ([ ]);
    nextId = 1;
    set_property(AUDIT_PROP, ({ }));
    /* Merry compiles after this domain (alphabetical initd order);
     * defer the observer binding until the boot iteration completes. */
    call_out("bind_audit_observer", 0);
}

/*
 * the audit trail is a Merry reaction: fires synchronously inside
 * every EVENT_PROP write, appending the event to AUDIT_PROP on this
 * same host. The same script routes the event to the SSE broker
 * (sys/streamd, the "stream" script space); the broker turns it into
 * zero-delay push call_outs, so an aborted write rolls the pushes
 * back with the mutation
 */
static void bind_audit_observer()
{
    MERRY->register_observer(this_object(), EVENT_PROP, "main",
	"Set($this, \"" + AUDIT_PROP + "\", " +
	"Get($this, \"" + AUDIT_PROP + "\") + ({ $new })); " +
	"stream::audit($entry: $new); return TRUE;");
}

/*
 * create an item owned by the authenticated principal; mutation and
 * audit event commit atomically
 */
atomic int create_item(string name, int qty, string creator)
{
    int id;

    id = nextId++;
    items[id] = ([ "name" : name, "qty" : qty, "creator" : creator ]);
    set_property(EVENT_PROP, "create " + id + " by " + creator);
    return id;
}

/*
 * update an item. Application-tier authorization: only the creator may
 * update. 1 = updated, 0 = no such item, -1 = not the creator.
 */
atomic int update_item(int id, string name, int qty, string principal)
{
    mapping item;

    item = items[id];
    if (!item) {
	return 0;
    }
    if (item["creator"] != principal) {
	return -1;
    }
    item["name"] = name;
    item["qty"] = qty;
    set_property(EVENT_PROP, "update " + id + " by " + principal);
    return 1;
}

/*
 * the capability-gated admin operation: wipe the whole inventory.
 * 1 = wiped, -1 = principal does not hold the platform capability.
 */
atomic int wipe(string principal)
{
    if (!principal ||
	!CAPABILITYD->is_allowed(ADMIN_CAPABILITY, principal)) {
	return -1;
    }
    items = ([ ]);
    set_property(EVENT_PROP, "wipe by " + principal);
    return 1;
}

/*
 * item by id (a copy), or nil
 */
mapping query_item(int id)
{
    return items[id] ? items[id] + ([ ]) : nil;
}

/*
 * all items as ({ ({ id, name, qty, creator }) ... }), ordered by id
 */
mixed *query_items()
{
    mixed *out;
    int *ids, i;

    ids = map_indices(items);
    out = allocate(sizeof(ids));
    for (i = 0; i < sizeof(ids); i++) {
	out[i] = ({ ids[i], items[ids[i]]["name"], items[ids[i]]["qty"],
		    items[ids[i]]["creator"] });
    }
    return out;
}

/*
 * the audit trail (the observer-written property)
 */
mixed *query_audit()
{
    mixed audit;

    audit = query_raw_property(AUDIT_PROP);
    return (typeof(audit) == T_ARRAY) ? audit[..] : ({ });
}

/*
 * the delegable-capability-gated read: an inventory summary for
 * subjects holding REPORT_CAPABILITY -- the same is_allowed
 * choke-point as wipe(), gating a different capability. The demo
 * pre-provisions this one delegable, so the principal holds it from
 * registration and an agent exactly while a delegation stands.
 * Defined after query_audit(), which it calls (LPC resolves calls
 * textually; there are no forward references without declarations).
 * -1 = the subject does not hold it.
 */
mixed report(string principal)
{
    if (!principal ||
	!CAPABILITYD->is_allowed(REPORT_CAPABILITY, principal)) {
	return -1;
    }
    return ([ "items" : map_sizeof(items),
	      "audit" : sizeof(query_audit()),
	      "nextId" : nextId ]);
}
