# include <kernel/kernel.h>
# include <kernel/user.h>
# include <status.h>
# include "/usr/HTTP/api/include/HttpConnection.h"

inherit "/usr/System/lib/auto";

# define APP_SERVER	"/usr/WWW/obj/server"

object userd;	/* kernel user daemon */

/*
 * register as binary-port manager on the first binary port
 */
static void create()
{
    ::create();
    userd = find_object(USERD);
    userd->set_binary_manager(0, this_object());
}

/*
 * select a per-connection application server for an incoming connection.
 * Applications mount a clonable server at /usr/WWW/obj/server (the
 * kernel-layer convention). The application server typically inherits
 * /usr/HTTP/api/lib/Server1 (the HTTP/1 connection library) and
 * /usr/System/lib/user (the binary-manager contract), and overrides
 * receiveRequest to handle routing. Inheriting /usr/HTTP/api/obj/server1
 * directly is not possible because DGD's inherit_program requires the
 * inherited path to contain /lib/. Existence is probed via
 * status(path, O_INDEX) rather than find_object() because System auto's
 * find_object refuses /obj/ masters to user-tier callers. If no
 * application server is mounted, the connection is dropped.
 */
object select(string str)
{
    object obj;

    if (previous_object() == userd && status(APP_SERVER, O_INDEX) != nil) {
	catch (obj = clone_object(APP_SERVER));
	return obj;
    }
}

/*
 * return the connection mode for a freshly selected server
 */
int query_mode(object obj)
{
    return MODE_LINE;
}

/*
 * return the connection-login timeout
 */
int query_timeout(object obj)
{
    return DEFAULT_TIMEOUT;
}
