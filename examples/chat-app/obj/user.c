/*
 * Chat-user clonable.
 *
 * One clone per user account. Holds Vault-style property storage for
 * name, presence, room subscriptions, mention-tracker, and the
 * admin-token list that backs the capability check inside sys/admin.
 *
 * query_state_root() returns "ChatApp:User" -- forward-compat for the
 * PD-2 persistence demonstration. PD-1 exercises only the in-memory
 * admin-token surface.
 *
 * Per architecture.md: clonables live under obj/, LWOs under data/.
 */

# include <type.h>

inherit "/lib/util/named";
inherit "/lib/util/lpc";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";

string query_state_root() { return "ChatApp:User"; }

static void create()
{
    properties::create();
    ur::create();
    set_property("chat-user.presence", ([ ]));
    set_property("chat-user.room-subscriptions", ({ }));
    set_property("chat-user.mention-tracker", ({ }));
    set_property("chat-user.admin-tokens", ({ }));
}

string  query_chat_name()           { return query_raw_property("chat-user.name"); }
void    set_chat_name(string n)     { set_property("chat-user.name", n); }

object *query_subscriptions()       { return query_raw_property("chat-user.room-subscriptions"); }
mapping query_presence()            { return query_raw_property("chat-user.presence"); }
mixed  *query_mention_tracker()     { return query_raw_property("chat-user.mention-tracker"); }
mixed  *query_admin_tokens()        { return query_raw_property("chat-user.admin-tokens"); }

void add_subscription(object room)
{
    object *current;

    if (!room) error("add_subscription: nil room");
    current = query_subscriptions();
    if (!member(room, current)) {
	set_property("chat-user.room-subscriptions", current + ({ room }));
    }
}

void remove_subscription(object room)
{
    object *current;

    if (!room) error("remove_subscription: nil room");
    current = query_subscriptions();
    set_property("chat-user.room-subscriptions", current - ({ room }));
}

void add_admin_token(mixed token)
{
    mixed *current;

    if (!token) error("add_admin_token: nil token");
    current = query_admin_tokens();
    set_property("chat-user.admin-tokens", current + ({ token }));
}
