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
