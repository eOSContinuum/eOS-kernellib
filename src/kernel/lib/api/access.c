# include <kernel/kernel.h>
# include <kernel/access.h>

private object access_daemon;		/* access manager */

/*
 * NAME:	create()
 * DESCRIPTION:	initialize API
 */
static void create()
{
    access_daemon = find_object(ACCESS_DAEMON);
}

/*
 * NAME:	access()
 * DESCRIPTION:	check access
 */
static int access(string user, string file, int type)
{
    if (!user || !file || type < 0 || type > FULL_ACCESS) {
	error("Bad arguments for access");
    }
    return access_daemon->access(user, file, type);
}

/*
 * NAME:	add_user()
 * DESCRIPTION:	add a new user
 */
static void add_user(string user)
{
    if (!user || sscanf(user, "/%*s") != 0) {
	error("Bad argument for add_user");
    }
    access_daemon->add_user(user);
}

/*
 * NAME:	remove_user()
 * DESCRIPTION:	remove a user
 */
static void remove_user(string user)
{
    if (!user) {
	error("Bad argument for remove_user");
    }
    access_daemon->remove_user(user);
}

/*
 * NAME:	query_users()
 * DESCRIPTION:	return list of users
 */
static string *query_users()
{
    return access_daemon->query_users();
}

/*
 * NAME:	save_access()
 * DESCRIPTION:	save access state to file
 */
static void save_access()
{
    access_daemon->save();
}

/*
 * NAME:	set_access()
 * DESCRIPTION:	set access
 */
static void set_access(string user, string file, int type)
{
    if (!user || !file || type < 0 || type > FULL_ACCESS) {
	error("Bad arguments for set_access");
    }
    access_daemon->set_access(user, file, type);
}

/*
 * NAME:	query_user_access()
 * DESCRIPTION:	get all access for a user
 */
static mapping query_user_access(string user)
{
    if (!user) {
	error("Bad argument for query_user_access");
    }
    return access_daemon->query_user_access(user);
}

/*
 * NAME:	query_file_access()
 * DESCRIPTION:	get all access to a path
 */
static mapping query_file_access(string path)
{
    if (!path) {
	error("Bad argument for query_file_access");
    }
    return access_daemon->query_file_access(path);
}

/*
 * NAME:	set_global_access()
 * DESCRIPTION:	set global read access for a directory
 */
static void set_global_access(string dir, int flag)
{
    if (!dir || (flag & ~1) != 0) {
	error("Bad arguments for set_global_access");
    }
    access_daemon->set_global_access(dir, flag);
}

/*
 * NAME:	query_global_access()
 * DESCRIPTION:	return the directories under /usr where everyone has read
 *		access
 */
static string *query_global_access()
{
    return access_daemon->query_global_access();
}
