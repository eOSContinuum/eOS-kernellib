/*
 * Route registry for the composite example: the runnable form of the
 * multi-application pattern sketched in docs/http-applications.md
 * "Multiple applications on one port".
 *
 * The platform mount point is a single path (/usr/WWW/obj/server for
 * HTTP, /usr/WWW/obj/tls_server for HTTPS), so running more than one
 * logical application behind one port means the WWW domain dispatches
 * by route prefix to handlers other domains register here at boot
 * (via the call_out-0 cross-domain registration idiom -- see
 * examples/composite-app/Inventory/initd.c).
 *
 * The registry is deliberately small: a prefix maps to a handler
 * object PATH, not an object reference, so a handler recompile or
 * upgrade never leaves a stale reference here; the server resolves
 * the path per request with find_object. Registration is open to any
 * domain -- the tier-E surface is cooperative, and the walkthrough
 * doc covers what a deployment that needs stronger route ownership
 * would add.
 */

# include <kernel/kernel.h>
# include <type.h>

inherit "/usr/System/lib/auto";

private mapping routes;		/* route prefix : handler object path */

static void create()
{
    ::create();
    routes = ([ ]);
}

/*
 * register a handler object path under a route prefix. The prefix
 * must start with "/" and not end with one; the longest registered
 * prefix wins at dispatch time.
 */
void register(string prefix, string handler)
{
    if (!prefix || prefix == "" || prefix[0] != '/' ||
	(strlen(prefix) > 1 && prefix[strlen(prefix) - 1] == '/')) {
	error("router: bad prefix");
    }
    if (!handler || handler == "" || handler[0] != '/') {
	error("router: bad handler path");
    }
    routes[prefix] = handler;
}

/*
 * drop a prefix registration
 */
void unregister(string prefix)
{
    routes[prefix] = nil;
}

/*
 * the handler object path for a request path: the longest registered
 * prefix that matches at a path-segment boundary, or nil
 */
string query_handler(string path)
{
    string *prefixes, best;
    int i, len;

    if (!path || path == "") {
	return nil;
    }
    best = nil;
    prefixes = map_indices(routes);
    for (i = 0; i < sizeof(prefixes); i++) {
	len = strlen(prefixes[i]);
	if (strlen(path) >= len && path[.. len - 1] == prefixes[i] &&
	    (strlen(path) == len || path[len] == '/')) {
	    if (!best || len > strlen(best)) {
		best = prefixes[i];
	    }
	}
    }
    return best ? routes[best] : nil;
}

/*
 * the current registrations (a copy)
 */
mapping query_routes()
{
    return routes + ([ ]);
}
