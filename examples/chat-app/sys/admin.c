/*
 * Admin daemon -- capability-gated chat-room moderation.
 *
 * Demonstrates capability separation via the per-user admin-token
 * LWO at /usr/Chat/data/admin_token. Each admin verb (kick / ban /
 * set_room_config) calls _check_admin_token(actor, room, action) at
 * entry; the check walks the actor's chat-user.admin-tokens property
 * and matches by (room, action). No matching token -> the verb
 * errors before any state mutation.
 *
 * grant_admin issues a new admin-token LWO and attaches it to the
 * subject user's admin-tokens list. The grantor must itself hold a
 * "grant" capability in the target room (or realm-wide via room=nil)
 * unless grantor is nil -- the nil-grantor path is reserved for the
 * test driver's bootstrap mint and is gated by the caller-program
 * convention rather than a token check (the test driver lives under
 * /usr/Chat/sys/, the same domain as this daemon, so the existing
 * domain-isolation discipline is sufficient as a first-revision
 * scope).
 *
 * _check_admin_token mirrors the shape of /usr/Merry/sys/merry's
 * _check_registrar capability gate: a private helper that takes the
 * inputs the public LFUN captured at entry, throws on rejection.
 *
 * Lives at /usr/Chat/sys/admin. One master, no clones.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/Chat/lib/app";
inherit "/usr/System/lib/auto";
inherit "/lib/util/lpc";

# define CHAT_DAEMON "/usr/Chat/sys/chat"
# define TOKEN_LWO   "/usr/Chat/data/admin_token"

private void _check_admin_token(object actor, object room, string action);

static void create()
{
    ::create();
}

/*
 * Public LFUN: kick a target user out of a room. Requires the actor
 * to hold an admin-token covering (room, "kick"). On success the
 * target is removed from the room's member list and the room is
 * removed from the target's subscription list.
 */
void kick(object actor, object target, object room)
{
    _check_admin_token(actor, room, "kick");
    if (!target) error("kick: nil target");
    if (!room) error("kick: nil room");
    CHAT_DAEMON->leave_room(target, room);
}

/*
 * Public LFUN: ban a target user from a room. Same shape as kick
 * but the target also gets the room added to a ban list. The ban
 * list is stored as a per-room property; future demonstrations may
 * extend the gate so a banned user cannot re-join.
 */
void ban(object actor, object target, object room)
{
    mixed *banned;

    _check_admin_token(actor, room, "ban");
    if (!target) error("ban: nil target");
    if (!room) error("ban: nil room");

    CHAT_DAEMON->leave_room(target, room);

    banned = room->query_raw_property("chat-room.banned");
    if (!banned) banned = ({ });
    if (!member(target, banned)) {
	room->set_property("chat-room.banned", banned + ({ target }));
    }
}

/*
 * Public LFUN: replace a room's config mapping. Capability-gated
 * under the "set-config" action.
 */
void set_room_config(object actor, object room, mapping config)
{
    _check_admin_token(actor, room, "set-config");
    if (!room) error("set_room_config: nil room");
    room->set_config(config);
}

/*
 * Public LFUN: mint an admin-token LWO and attach it to the subject's
 * admin-tokens list. The grantor must itself hold a "grant" action
 * for the room (or realm-wide). nil grantor is allowed as a bootstrap
 * path; the caller-program domain isolation is the current gate.
 */
mixed grant_admin(object grantor, object subject, object room, string *actions)
{
    mixed token;

    if (!subject) error("grant_admin: nil subject");
    if (!actions || sizeof(actions) == 0) {
	error("grant_admin: empty actions");
    }
    if (grantor) {
	_check_admin_token(grantor, room, "grant");
    }

    token = new_object(TOKEN_LWO);
    token->set_grantor(grantor);
    token->set_subject(subject);
    token->set_room(room);
    token->set_granted_at(time());
    token->set_actions(actions);
    subject->add_admin_token(token);
    return token;
}

/*
 * Capability check: walk the actor's admin-tokens list looking for a
 * token whose room matches (or is nil for realm-wide) AND whose
 * actions array contains the requested action. Throws on rejection;
 * returns silently on acceptance.
 */
private
void _check_admin_token(object actor, object room, string action)
{
    mixed *tokens;
    mixed token;
    int i, sz;

    if (!actor) {
	error("admin: nil actor for " + action);
    }
    tokens = actor->query_admin_tokens();
    sz = sizeof(tokens);
    for (i = 0; i < sz; i++) {
	token = tokens[i];
	if (!token) continue;
	if (token->query_room() != room && token->query_room() != nil) {
	    continue;
	}
	if (member(action, token->query_actions())) {
	    return;
	}
    }
    error("admin: actor " + (actor->query_chat_name() ?
	  actor->query_chat_name() : "(unnamed)") +
	  " not authorized for " + action + " in room " +
	  (room ? (room->query_id() ? room->query_id() : "(unnamed)") :
	   "(realm)"));
}
