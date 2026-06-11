/*
 * Logical name layer.
 *
 * Provides a mutable logical name distinct from DGD's immutable
 * object_name() kfun. Daemons and clones that need addressing by
 * a domain-meaningful identity ("Schema:Daemon", "Vault:Daemon",
 * "Schema:Element:Hierarchy") inherit this lib and call
 * set_object_name() at create() time.
 *
 * set_object_name() wires through to ~Index/sys/index_daemon
 * so the name -> object map is global and O(1)-invertible via
 * find_named(). The local _logical_name slot is kept as a cheap cache
 * for query_object_name() callers (avoids a daemon round-trip on every
 * read) and as a sentinel for the inheriting object's own
 * destruct-time bookkeeping; the index-side registration is cleared
 * from the kernel destruct hook (/kernel/lib/auto.c::destruct_object)
 * via Index->clear_name_for_object(), so consumers don't have to do
 * anything new.
 */

# define INDEX		"/usr/Index/sys/index_daemon"

private string _logical_name;

nomask string query_object_name()
{
    return _logical_name;
}

void set_object_name(string lname)
{
    if (_logical_name && _logical_name != lname) {
	INDEX->clear_name(_logical_name);
    }
    if (lname) {
	INDEX->set_name(this_object(), lname);
    }
    _logical_name = lname;
}

/*
 * NAME:	find_named()
 * DESCRIPTION:	logical-name -> object lookup. Returns nil if the name
 *		is not registered. Wraps Index->query_object so consumers
 *		that already inherit this lib don't need to know about the
 *		Index daemon path.
 */
object find_named(string lname)
{
    return INDEX->query_object(lname);
}
