/*
 * Admin-token Light-Weight Object (LWO).
 *
 * One token represents a single (subject, room, action-set) grant.
 * The capability check inside sys/admin walks the subject user's
 * `chat-user.admin-tokens` list and matches by room + action.
 *
 * The token's fields are set by sys/admin::grant_admin at issuance
 * and not mutated thereafter. LPC has no language-level immutability
 * for LWOs; the convention is to treat the token as immutable once
 * attached to a user.
 *
 * room == nil reserves the realm-wide admin scope; otherwise the
 * token applies to that specific Room clone only.
 *
 * actions is a string array, e.g. ({ "kick", "ban", "set-config" }).
 */

private object _grantor;
private object _subject;
private object _room;
private int _granted_at;
private string *_actions;

static void create()
{
}

void set_grantor(object g)         { _grantor = g; }
void set_subject(object s)         { _subject = s; }
void set_room(object r)            { _room = r; }
void set_granted_at(int t)         { _granted_at = t; }
void set_actions(string *a)        { _actions = a; }

object   query_grantor()    { return _grantor; }
object   query_subject()    { return _subject; }
object   query_room()       { return _room; }
int      query_granted_at() { return _granted_at; }
string  *query_actions()    { return _actions ? _actions[..] : ({ }); }
