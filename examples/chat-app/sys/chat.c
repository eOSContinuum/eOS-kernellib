/*
 * Chat dispatcher daemon.
 *
 * Application-tier daemon that drives room join/leave + message
 * posting. join_room sets up the membership state the test driver
 * exercises. post_message is the async-event entry point: it resolves
 * "@name" mentions against the room roster, then writes a
 * chat-room.message arrival signal via set_property. That single write
 * is what the dispatcher fans out -- a room's main-timing
 * append-to-history observer records the message, and (where
 * registered) a post-timing mention-notify observer schedules a
 * cross-user notification on a later tick. post_message itself does NOT
 * append to the message log; the append is an observer reaction, which
 * is what makes message arrival a first-class dispatched event rather
 * than a daemon side effect.
 *
 * Lives at /usr/Chat/sys/chat. One master, no clones.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/Chat/lib/app";
inherit "/usr/System/lib/auto";
inherit "/lib/util/lpc";

private object *scan_mentions(object room, string content);

static void create()
{
    ::create();
}

void join_room(object user, object room)
{
    if (!user || !room) error("join_room: nil arg");
    room->add_member(user);
    user->add_subscription(room);
}

void leave_room(object user, object room)
{
    if (!user || !room) error("leave_room: nil arg");
    room->remove_member(user);
    user->remove_subscription(room);
}

void post_message(object sender, object room, string content)
{
    mapping msg;

    if (!sender || !room) error("post_message: nil arg");
    if (!content) error("post_message: nil content");

    msg = ([
	"sender":    sender,
	"timestamp": time(),
	"content":   content,
	"mentions":  scan_mentions(room, content),
    ]);

    /* The arrival signal. set_property routes through the dispatcher:
     * the room's main-timing append-to-history observer appends this
     * mapping to chat-room.message-log, and a post-timing mention-notify
     * observer (where registered) schedules cross-user notification.
     * post_message returns as soon as the synchronous pre/main/post
     * fan-out completes; any $delay-scheduled notification fires on a
     * subsequent tick, decoupled from this call. */
    room->set_property("chat-room.message", msg);
}

/*
 * scan_mentions: resolve "@name" tokens in the message content to the
 * room-member user objects they name. Returns the matched users (the
 * mention list carried in the message mapping). Only current room
 * members are resolvable -- a mention of a non-member is dropped. The
 * scan is daemon-side (LPC) so the observer source stays a thin
 * reaction over structured mention data; the async fan-out the
 * observer performs is the async-event primitive, not the string parse.
 */
private object *scan_mentions(object room, string content)
{
    object *members, *hits;
    string *words;
    int i, j;

    members = room->query_members();
    hits = ({ });
    words = explode(content, " ");
    for (i = 0; i < sizeof(words); i ++) {
	string name;

	if (strlen(words[i]) < 2 || words[i][0] != '@') {
	    continue;
	}
	name = words[i][1 ..];
	for (j = 0; j < sizeof(members); j ++) {
	    if (members[j]->query_chat_name() == name &&
		!member(members[j], hits)) {
		hits += ({ members[j] });
	    }
	}
    }
    return hits;
}
