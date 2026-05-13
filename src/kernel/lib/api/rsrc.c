# include <kernel/kernel.h>
# include <kernel/rsrc.h>
# include <type.h>

private object resource_daemon;		/* resource manager */

/*
 * NAME:	create()
 * DESCRIPTION:	initialize API
 */
static void create()
{
    resource_daemon = find_object(RESOURCE_DAEMON);
}

/*
 * NAME:	add_owner()
 * DESCRIPTION:	add a new resource owner
 */
static void add_owner(string owner)
{
    resource_daemon->add_owner(owner);
}

/*
 * NAME:	remove_owner()
 * DESCRIPTION:	remove a resource owner
 */
static void remove_owner(string owner)
{
    resource_daemon->remove_owner(owner);
}

/*
 * NAME:	query_owners()
 * DESCRIPTION:	return a list of resource owners
 */
static string *query_owners()
{
    return resource_daemon->query_owners();
}


/*
 * NAME:	set_rsrc()
 * DESCRIPTION:	set the maximum, decay percentage and decay period of a
 *		resource
 */
static void set_rsrc(string name, int max, int decay, int period)
{
    if (!name || max < -1 || decay < 0 || decay > 100 || period < 0 ||
	!decay != !period) {
	error("Bad arguments for set_rsrc");
    }
    resource_daemon->set_rsrc(name, max, decay, period);
}

/*
 * NAME:	remove_rsrc()
 * DESCRIPTION:	remove a resource
 */
static void remove_rsrc(string name)
{
    if (!name) {
	error("Bad argument for remove_rsrc");
    }
    resource_daemon->remove_rsrc(name);
}

/*
 * NAME:	query_rsrc()
 * DESCRIPTION:	get usage and limits of a resource
 */
static mixed *query_rsrc(string name)
{
    if (!name) {
	error("Bad argument for query_rsrc");
    }
    return resource_daemon->query_rsrc(name);
}

/*
 * NAME:	query_resources()
 * DESCRIPTION:	return a list of resources
 */
static string *query_resources()
{
    return resource_daemon->query_resources();
}


/*
 * NAME:	rsrc_set_limit()
 * DESCRIPTION:	set individual resource limit
 */
static void rsrc_set_limit(string owner, string name, int max)
{
    if (!name || max < -1) {
	error("Bad arguments for rsrc_set_limit");
    }
    resource_daemon->rsrc_set_limit(owner, name, max);
}

/*
 * NAME:	rsrc_set_maxtickusage()
 * DESCRIPTION:	set maximum tick usage
 */
static void rsrc_set_maxtickusage(string owner, float tickusage)
{
    if (tickusage < -1.0) {
	error("Bad argument for rsrc_set_maxtickusage");
    }
    resource_daemon->rsrc_set_maxtickusage(owner, tickusage);
}

/*
 * NAME:	rsrc_get()
 * DESCRIPTION:	get individual resource usage
 */
static mixed *rsrc_get(string owner, string name)
{
    if (!name) {
	error("Bad arguments for rsrc_get");
    }
    return resource_daemon->rsrc_get(owner, name);
}

/*
 * NAME:	rsrc_get_maxtickusage()
 * DESCRIPTION:	get maximum tick usage
 */
static float rsrc_get_maxtickusage(string owner)
{
    return resource_daemon->rsrc_get_maxtickusage(owner);
}

/*
 * NAME:	rsrc_incr()
 * DESCRIPTION:	increment or decrement a resource
 */
static void rsrc_incr(string owner, string name, int incr)
{
    if (!name) {
	error("Bad arguments for rsrc_incr");
    }
    resource_daemon->rsrc_incr(owner, name, incr);
}
