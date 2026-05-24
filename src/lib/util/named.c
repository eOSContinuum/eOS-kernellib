/*
 * Logical name layer.
 *
 * Provides a mutable logical name distinct from DGD's immutable
 * object_name() kfun. Daemons and clones that need addressing by
 * a domain-meaningful identity ("Schema:Daemon", "Vault:Daemon",
 * "Schema:Element:Hierarchy") inherit this lib and call
 * set_object_name() at create() time.
 *
 * Stub stage: name is stored privately and returned by
 * query_object_name(). LV-4.5d lifts Index (IDD) and rewires
 * set_object_name() to maintain a global name -> object map for
 * O(1) inverse lookup. The API surface defined here is stable
 * across that transition; consumers will not need to change.
 */

private string _logical_name;

nomask string query_object_name()
{
    return _logical_name;
}

void set_object_name(string lname)
{
    _logical_name = lname;
}
