/*
 * Ur-object infrastructure (parent + child tracking).
 *
 * Lifted from SkotOS /lib/ur.c per LM-3.5. Strips at lift:
 *   # include <adt.h>            -> dropped (only contained URCHILD_ITERATOR)
 *   query_ur_child_iterator()    -> dropped (depends on /lib/bigmap_iterator
 *                                   + /data/urchild_iterator LWO; not used
 *                                   by Merry's find_merry ancestry walk)
 *   XDebug(...)                  -> dropped (debug-no-op per L14 #5)
 *   SysLog(...)                  -> dropped (no log facility yet per LV-4.5b;
 *                                   the two SysLog calls were warning paths
 *                                   in delete_child / add_child and are
 *                                   informational only)
 *   ur_name(child) helper        -> inlined as ::object_name(child) per
 *                                   SkotOS usr/System/lib/sys_auto.c::ur_name
 *
 * Inheriting this lib makes an object both an ur-parent and an ur-child.
 * Merry's find_merry / find_merries / run_merry walk the chain via
 * query_ur_object() to look up the merry:<mode>:<signal> property at
 * each ancestor.
 */

# include <status.h>


private object ur;            /* our ur-parent */

/* the two-level mapping of children, keyed by clone-index / 1024 */
private mapping childmap;
private int childcount;

private void unlink();
private void link();


atomic
void patch_child_child()
{
    if (ur) {
	ur->update_child(this_object());
    }
}

atomic
void patch_map()
{
    if (!childmap) {
	childmap = ([ ]);
	childcount = 0;
    }
}

atomic
void patch_urchildren()
{
    patch_map();
    if (ur && !ur->is_immediate_child(this_object())) {
	ur->add_child(this_object());
    }
}

static
void create()
{
    childmap = ([ ]);
}

static
void destructive_desire()
{
    if (childcount > 0 && map_sizeof(childmap)) {
	error("cannot destruct ur-object with ur-children");
    }
}

static atomic
void destructing()
{
    if (ur) {
	unlink();
    }
}

atomic nomask
void set_ur_object(object ob)
{
    object tmp;

    if (ur == ob) {
	return;
    }
    for (tmp = ob; tmp; tmp = tmp->query_ur_object()) {
	if (tmp == this_object()) {
	    error("would cause ur-object cycle");
	}
    }
    if (ur) {
	unlink();
    }
    tmp = ur;

    ur = ob;

    if (ur) {
	link();
	catch {
	    this_object()->ur_object_set(ur, tmp);
	}
    }
}

nomask
object query_ur_object()
{
    return ur;
}

private void unlink()
{
    ur->release_ur_child(this_object());
}

private void link()
{
    ur->adopt_ur_child(this_object());
}

nomask atomic
void update_child(object child)
{
    mapping row;
    int ix;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();
    row = childmap[ix / 1024];
    if (!row) {
	error("internal error");
    }
    if (child->query_first_child()) {
	row[child] = -1;
    } else {
	row[child] = time();
    }
}


/* three functions to be called in an ur object */
int is_immediate_child(object child)
{
    mapping row;
    int ix;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();
    row = childmap[ix / 1024];
    return row && row[child];
}

nomask atomic
void delete_child(object child)
{
    mapping row;
    int ix;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();
    row = childmap[ix / 1024];
    if (row && row[child]) {
	childcount --;
    }
    if (row) {
	row[child] = nil;
	if (!map_sizeof(row)) {
	    childmap[ix / 1024] = nil;
	}
    }
    if (ur && childcount == 0) {
	ur->child_loses_children(this_object());
    }
}

nomask atomic
void add_child(object child)
{
    mapping row;
    int ix;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();
    row = childmap[ix / 1024];
    if (!row) {
	row = childmap[ix / 1024] = ([ ]);
    }

    if (!row[child]) {
	childcount ++;
    }

    update_child(child);

    if (ur && childcount == 1) {
	ur->child_acquires_children(this_object());
    }
}

nomask
void release_ur_child(object child)
{
    if (previous_program() == "/lib/util/ur") {
	delete_child(child);
    }
}

nomask
void adopt_ur_child(object child)
{
    if (previous_program() == "/lib/util/ur") {
	add_child(child);
    }
}

/* called in A's ur-parent when A loses its last ur-child */
nomask
void child_loses_children(object child)
{
    if (previous_program() == "/lib/util/ur") {
	update_child(child);
    }
}

/* called in A's ur-parent when A acquires its first ur-child */
nomask
void child_acquires_children(object child)
{
    if (previous_program() == "/lib/util/ur") {
	update_child(child);
    }
}


nomask
int query_child_count()
{
    return childcount;
}

nomask
int is_child_with_children(object child)
{
    mapping row;
    int ix;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();
    row = childmap[ix / 1024];
    if (!row) {
	error("not a child");
    }
    return row[child] == -1;
}


/* debug */
nomask
mapping query_child_mapping()
{
    return childmap;
}

object *query_ur_children()
{
    int     i, sz;
    object  *list;
    mapping *maps;

    if (childcount > status()[ST_ARRAYSIZE]) {
	error("More ur-children than can fit in an array");
    }
    list = ({ });
    sz = map_sizeof(childmap);
    maps = map_values(childmap);
    for (i = 0; i < sz; i++) {
	list += map_indices(maps[i]);
    }
    return list;
}

int is_child_of(object ancestor)
{
    if (!ur) {
	return FALSE;
    }
    if (ur == ancestor) {
	return TRUE;
    }
    return ur->is_child_of(ancestor);
}

nomask
object query_next_ur_sibling()
{
    if (ur) {
	return ur->query_next_ur_child(this_object());
    }
}

nomask
object query_previous_ur_sibling()
{
    if (ur) {
	return ur->query_previous_ur_child(this_object());
    }
}

nomask
object query_first_child()
{
    if (childcount > 0) {
	int i, sz;
	mapping *maps;

	sz   = map_sizeof(childmap);
	maps = map_values(childmap);
	for (i = 0; i < sz; i++) {
	    if (map_sizeof(maps[i]) > 0) {
		return map_indices(maps[i])[0];
	    }
	}
    }
    return nil;
}

nomask
object query_next_ur_child(object child)
{
    object *list;
    int ix, sz;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();

    if (childcount == 1) {
	return child;
    }
    ix = ix / 1024;
    sz = 1 + map_indices(childmap)[map_sizeof(childmap) - 1];
    list = map_indices(childmap[ix][child ..]);
    while (sizeof(list) < 2) {
	ix = (ix + 1) % sz;
	if (childmap[ix]) {
	    list += map_indices(childmap[ix]);
	}
    }
    return list[1];
}

nomask
object query_previous_ur_child(object child)
{
    object *list;
    int ix, sz;

    if (!sscanf(::object_name(child), "%*s#%d", ix)) {
	error("not a clone");
    }
    patch_map();

    if (childcount == 1) {
	return child;
    }
    ix = ix / 1024;
    sz = 1 + map_indices(childmap)[map_sizeof(childmap) - 1];
    list = map_indices(childmap[ix][.. child]);
    while (sizeof(list) < 2) {
	ix = (ix + sz - 1) % sz;
	if (childmap[ix]) {
	    list = map_indices(childmap[ix]) + list;
	}
    }
    return list[sizeof(list) - 2];
}
