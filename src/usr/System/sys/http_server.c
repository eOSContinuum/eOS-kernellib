# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>
# include <status.h>
# include <portd.h>
# include "/usr/HTTP/api/include/HttpConnection.h"

inherit "/usr/System/lib/auto";
inherit "/kernel/lib/capability";

# define APP_SERVER	"/usr/WWW/obj/server"

/*
 * register as binary-port manager on the labeled HTTP port
 */
static void create()
{
    ::create();
    PORTD->register_manager("http", this_object());
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
    object obj, po;

    /*
     * The accept gate, routed through the capability choke-point: only
     * the binary-port manager (userd) may have a connection accepted.
     * is_allowed is the silent boolean path -- an unauthorized caller
     * gets a dropped connection (nil), not an error: erroring on every
     * unauthorized accept (probes included) would buy nothing. The
     * principal is the manager's object name; the prior
     * previous_object() == userd identity check is the same test
     * expressed against the store.
     */
    po = previous_object();
    if (po && is_allowed("http.binary_manager", object_name(po)) &&
	status(APP_SERVER, O_INDEX) != nil) {
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
