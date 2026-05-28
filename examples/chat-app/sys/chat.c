/*
 * Chat dispatcher daemon.
 *
 * Application-tier daemon that drives room join/leave + message
 * posting. PD-1 exercises join_room (used by the test driver to set
 * up the membership state that admin->kick will modify). post_message
 * is defined for completeness but its dispatcher hookup lands at
 * PD-4 when async-event semantics enter scope.
 *
 * Lives at /usr/Chat/sys/chat. One master, no clones.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/Chat/lib/app";
inherit "/usr/System/lib/auto";

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
    mixed *log;
    mapping msg;

    if (!sender || !room) error("post_message: nil arg");
    if (!content) error("post_message: nil content");

    msg = ([
	"sender":    sender,
	"timestamp": time(),
	"content":   content,
	"mentions":  ({ }),
    ]);

    log = room->query_messages();
    /* Direct property write -- PD-4 will route this through the
     * dispatcher so post-timing observers can fire mention-scan and
     * cross-user notifications. At PD-1 the property is set
     * directly; no observers are registered yet. */
    room->set_property("chat-room.message-log", log + ({ msg }));
}
