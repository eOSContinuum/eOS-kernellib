/*
 * Schema node clonable.
 *
 * Thin wrapper inheriting the schema_node library that schema_daemon
 * clones to instantiate each named schema element. The wrapper sets
 * its object_name so the runtime registry can find clones by their
 * "Schema:<namespace>:<tag>" identity.
 *
 * Object names carry the Schema: prefix.
 * (b) queryStateRoot() result renamed SID:Element -> Schema:Element
 *     (the schema-element-FOR-schema-elements -- this is the self-
 *     describing schema node that lives at the root of the Schema
 *     subsystem's own type tree).
 */

inherit "/usr/Schema/lib/schema_node";

string queryStateRoot() { return "Schema:Element"; }

static void create()
{
    ::create();
    if (sscanf(object_name(this_object()), "%*s#") == 0) {
	/* master: not a clone, set its logical name */
	::set_object_name("Schema:UrNode");
    }
}

nomask void set_object_name(string name)
{
    if (query_namespace() || query_tag()) {
	error("configured schema nodes cannot be renamed");
    }
    /* but do allow the naming of a virgin node */
    ::set_object_name(name);
}

atomic void set_name(string s, string t)
{
    if (::set_name(s, t)) {
	::set_object_name("Schema:" + s + ":" + t);
    }
}
