/*
 * Chat-room clonable.
 *
 * One clone per room. Holds Vault-style property storage for member
 * list, message log, and per-room config. Property names use the
 * "chat-room." prefix to namespace them off the room ancestry.
 *
 * query_state_root() returns "ChatApp:Room" -- a future Schema
 * registration could bind this clonable into the marshaler when a
 * later revision exercises the persistence path. The first
 * revision's scope is the capability-gated admin verbs;
 * property-storage round-tripping exercises subsequently.
 *
 * Per architecture.md: clonables live under obj/, LWOs under data/,
 * daemons under sys/.
 */

# include <type.h>

inherit "/lib/util/named";
inherit "/lib/util/lpc";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";
inherit "/lib/util/delayed";

string query_state_root() { return "ChatApp:Room"; }

static void create()
{
    properties::create();
    ur::create();
    set_property("chat-room.member-list", ({ }));
    set_property("chat-room.message-log", ({ }));
    set_property("chat-room.config", ([ ]));
}

string  query_id()           { return query_raw_property("chat-room.id"); }
void    set_id(string id)    { set_property("chat-room.id", id); }

object *query_members()      { return query_raw_property("chat-room.member-list"); }
mixed  *query_messages()     { return query_raw_property("chat-room.message-log"); }
mapping query_config()       { return query_raw_property("chat-room.config"); }

void add_member(object user)
{
    object *current;

    if (!user) error("add_member: nil user");
    current = query_members();
    if (!member(user, current)) {
	set_property("chat-room.member-list", current + ({ user }));
    }
}

void remove_member(object user)
{
    object *current;

    if (!user) error("remove_member: nil user");
    current = query_members();
    set_property("chat-room.member-list", current - ({ user }));
}

void set_config(mapping config)
{
    set_property("chat-room.config", config);
}

void    set_capacity(int n)  { set_property("chat-room.capacity", n); }
int     query_capacity()     { return query_raw_property("chat-room.capacity"); }

/*
 * claim_slot: take a slot in a capacity-bounded room, serialized.
 *
 * Re-reads the CURRENT member list at call time. Because each call runs
 * as its own atomic DGD task, a second caller's task observes the first
 * caller's committed membership and is refused when the room is already
 * full. Returns 1 if the slot was claimed, 0 if the room was at capacity.
 * There is no lock and no coordination protocol -- the runtime's
 * coherent-state read (current members, read at write time) is the
 * serialization. Capacity <= 0 means unbounded.
 */
int claim_slot(object user)
{
    object *current;
    int cap;

    if (!user) error("claim_slot: nil user");
    cap = query_capacity();
    current = query_members();
    if (cap > 0 && sizeof(current) >= cap) {
	return 0;
    }
    if (!member(user, current)) {
	set_property("chat-room.member-list", current + ({ user }));
    }
    return 1;
}

/*
 * claim_slot_stale: the lost update claim_slot prevents.
 *
 * Writes the member list from a STALE snapshot captured before another
 * claim committed, so this write overwrites the other caller's addition.
 * Present only to contrast claim_slot: it makes visible the lost update
 * that appears when a writer acts on state read BEFORE a concurrent
 * commit instead of re-reading at write time. Real code uses claim_slot.
 */
void claim_slot_stale(object user, object *stale_snapshot)
{
    if (!user) error("claim_slot_stale: nil user");
    set_property("chat-room.member-list", stale_snapshot + ({ user }));
}

/* A post-timing mention-notify observer registered on this room uses
 * $delay() to push its cross-user notification onto a later tick. The
 * delayed_call / perform_delayed_call pair Merry's do_delay requires of
 * the dispatch host lives in /lib/util/delayed, inherited above. */
