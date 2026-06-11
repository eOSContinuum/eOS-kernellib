/*
 * ~Index/sys/index_daemon.c
 *
 * Second-level naming system. Maintains a logical name -> object map
 * for O(1) inverse lookup, complementing DGD's path-based object_name()
 * and find_object() kfuns. Names are colon-delimited hierarchical paths
 * (e.g. "Schema:Core:Entry", "Vault:Daemon"). The tree itself is a
 * nested mapping; a parallel object -> name map supports destruct-time
 * cleanup without the caller knowing the registered name.
 *
 * Name-change event notifications are not part of this surface;
 * introspection is via direct method calls (query_tree, query_subdirs,
 * query_objects). set_name is gated to previous_program() ==
 * "/lib/util/named"; the destruct entry point clear_name_for_object()
 * is KERNEL()-gated so only /kernel/lib/auto's destruct_object can
 * invoke it. The daemon owns the names_by_object reverse map so
 * destruct can clear a registration without the destruct site knowing
 * the name.
 */

# include <type.h>
# include <kernel/kernel.h>

# define NAMED		"/lib/util/named"

mapping tree;			/* nested name tree: levels keyed by path segment */
mapping names_by_object;	/* object -> registered name (reverse map) */


static void create()
{
    tree = ([ ]);
    names_by_object = ([ ]);
}


/*
 * NAME:	set_name()
 * DESCRIPTION:	register a logical name for an object. Caller is the
 *		named lib at /lib/util/named, invoked from a consumer's
 *		set_object_name() at create() time.
 */
atomic void set_name(object ob, string name)
{
    mapping lvl;
    string *paths;
    int i;

    if (previous_program() != NAMED) {
	return;
    }
    if (!name || !strlen(name)) {
	error("bad object name");
    }
    if (name[0] == '/') {
	error("object name cannot start with '/'");
    }

    lvl = tree;
    paths = explode(name, ":");
    for (i = 0; i < sizeof(paths) - 1; i++) {
	if (!strlen(paths[i])) {
	    error("names including :: are forbidden");
	}
	switch (typeof(lvl[paths[i]])) {
	case T_NIL:
	    lvl = lvl[paths[i]] = ([ ]);
	    break;
	case T_MAPPING:
	    lvl = lvl[paths[i]];
	    break;
	case T_OBJECT:
	    error("Index: can't create subdir " +
		  implode(paths[..i], ":") + ": taken");
	}
    }
    switch (typeof(lvl[paths[i]])) {
    case T_NIL:
	lvl[paths[i]] = ob;
	names_by_object[ob] = name;
	break;
    case T_OBJECT:
	if (lvl[paths[i]] != ob) {
	    error("Index: object name " + name + " is taken");
	}
	break;
    case T_MAPPING:
	error("Index: object name " + name + " already refers to a folder");
    default:
	error("Index: internal error for object name " + name);
    }
}


/*
 * NAME:	clear_name()
 * DESCRIPTION:	remove a registered logical name. Caller is the named
 *		lib, invoked from set_object_name(nil) or when a name
 *		is being replaced.
 */
atomic void clear_name(string name)
{
    mixed *lvl;
    string *paths;
    object ob;
    int i;

    if (previous_program() != NAMED) {
	return;
    }

    paths = explode(name, ":");
    lvl = allocate(sizeof(paths) + 1);
    lvl[0] = tree;

    for (i = 0; i < sizeof(paths); i++) {
	if (typeof(lvl[i]) != T_MAPPING) {
	    return;
	}
	lvl[i + 1] = lvl[i][paths[i]];
    }

    /* drop the reverse map entry for the named object */
    if (typeof(lvl[i]) == T_OBJECT) {
	ob = lvl[i];
	if (names_by_object[ob] == name) {
	    names_by_object[ob] = nil;
	}
    }

    /* unwind: drop empty mappings up the chain */
    while (i > 0) {
	if (typeof(lvl[i]) == T_MAPPING && map_sizeof(lvl[i]) > 0) {
	    return;
	}
	i--;
	lvl[i][paths[i]] = nil;
    }
}


/*
 * NAME:	clear_name_for_object()
 * DESCRIPTION:	destruct-time entry point for the kernel auto.c hook.
 *		Looks up the object's registered name (if any) and
 *		clears it.
 */
atomic void clear_name_for_object(object ob)
{
    string name;
    mixed *lvl;
    string *paths;
    int i;

    if (!KERNEL()) {
	return;
    }
    if (!ob) {
	return;
    }
    name = names_by_object[ob];
    if (!name) {
	return;
    }

    /* mirror the clear_name walk without the previous_program gate */
    paths = explode(name, ":");
    lvl = allocate(sizeof(paths) + 1);
    lvl[0] = tree;

    for (i = 0; i < sizeof(paths); i++) {
	if (typeof(lvl[i]) != T_MAPPING) {
	    names_by_object[ob] = nil;
	    return;
	}
	lvl[i + 1] = lvl[i][paths[i]];
    }

    names_by_object[ob] = nil;

    while (i > 0) {
	if (typeof(lvl[i]) == T_MAPPING && map_sizeof(lvl[i]) > 0) {
	    return;
	}
	i--;
	lvl[i][paths[i]] = nil;
    }
}


/*
 * NAME:	query_object()
 * DESCRIPTION:	logical-name -> object lookup. Returns nil if the name
 *		is unregistered or refers to a folder.
 */
object query_object(string name)
{
    mixed lvl;
    string *paths;
    int i;

    if (!name || !strlen(name)) {
	return nil;
    }

    lvl = tree;
    paths = explode(name, ":");

    for (i = 0; i < sizeof(paths) - 1; i++) {
	lvl = lvl[paths[i]];
	if (typeof(lvl) != T_MAPPING) {
	    return nil;
	}
    }
    if (typeof(lvl = lvl[paths[i]]) == T_OBJECT) {
	return lvl;
    }
    return nil;
}


/*
 * NAME:	query_name()
 * DESCRIPTION:	object -> logical-name lookup. Returns nil if the
 *		object has no registered name.
 */
string query_name(object ob)
{
    return names_by_object[ob];
}


/*
 * Introspection helpers. Walk the tree from a colon-delimited path
 * (nil/empty = root), returning child folders or child objects.
 */

mapping query_tree() { return tree; }

string *query_subdirs(varargs string path)
{
    mixed lvl;
    string *paths;
    mixed *entries;
    int i;

    lvl = tree;
    paths = path ? explode(path, ":") : ({ });

    for (i = 0; i < sizeof(paths); i++) {
	lvl = lvl[paths[i]];
	if (typeof(lvl) != T_MAPPING) {
	    return ({ });
	}
    }
    entries = map_indices(lvl);
    for (i = 0; i < sizeof(entries); i++) {
	if (typeof(lvl[entries[i]]) != T_MAPPING) {
	    entries[i] = nil;
	}
    }
    return entries - ({ nil });
}

string *query_objects(varargs string path)
{
    mixed lvl;
    string *paths;
    mixed *entries;
    int i;

    lvl = tree;
    paths = path ? explode(path, ":") : ({ });

    for (i = 0; i < sizeof(paths); i++) {
	lvl = lvl[paths[i]];
	if (typeof(lvl) != T_MAPPING) {
	    return ({ });
	}
    }
    entries = map_indices(lvl);
    for (i = 0; i < sizeof(entries); i++) {
	if (typeof(lvl[entries[i]]) != T_OBJECT) {
	    entries[i] = nil;
	}
    }
    return entries - ({ nil });
}
