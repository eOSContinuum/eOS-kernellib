/*
 * Schema daemon: coordinates the schema_node registry by namespace+tag.
 *
 * Each schema_node registers itself via set_name(ns, tag); the daemon
 * indexes them so callers can resolve element-name -> schema_node via
 * query_node() / get_node(). The schema_daemon also configures the
 * structural primitives (Element / Children / Attribute / Attributes /
 * Callback / Callbacks / Iterator) at boot -- the schema-FOR-schemas
 * tree -- and is the load target for additional domain schemas.
 *
 * Lifted from skoot/usr/SID/sys/sid.c. LV-4.5b refactors:
 * (a) /usr/SID/lib/vaultnode inherit dropped -- vault participation is a
 *     LV-4.5c concern (Marshal/stateimpex coordinates the vault path).
 *     The schema_daemon does NOT live in the vault tree at this lift.
 * (b) /usr/SID/lib/stateimpex inherit dropped for the same reason --
 *     stateimpex is the LV-4.5c lift target; schema_daemon delegates
 *     to it once it lands.
 * (c) /lib/string inherit replaced by /lib/util/ascii (lower_case).
 * (d) tool/vault->load_subdir(...) bootstrap calls dropped per BACKLOG
 *     instruction: schema_daemon must NOT depend on vault_ops.c. Core
 *     schemas are code-defined in configure_initial_nodes() (extended
 *     to cover the lifted Ur:Hierarchy / Ur:UrChild / Ur:UrChildren /
 *     Core:Entry / Core:Entries primitives with Note 4 renames
 *     applied). XML-file-driven loading via read_file + parse_xml is
 *     scaffolded as load_core_schemas() but deferred to LV-4.5c when
 *     stateimpex lands -- the XML files in data/schema/ reference the
 *     <object program=...> spawn format that stateimpex handles.
 * (e) Object-name "SID:Daemon" -> "Schema:Daemon" per LV-2 rename.
 *     SCHEMA_NODE constant inline-defined for the obj clonable path.
 * (f) Element names in configure_initial_nodes() renamed SID: -> Schema:
 *     so the schema-for-schemas tree lives in the Schema: namespace.
 *     Note 4 renames applied: Ur:UrObject -> Ur:Hierarchy with
 *     `urobject` attribute renamed to `parent`; Core:Property ->
 *     Core:Entry with `property` attribute renamed to `key`.
 * (g) INFO() logging stub replaced by sysLog() from /lib/util/lpc.
 *     SkotOS-style HARD/NREF and SID:* special-case for SkotOS:Socials
 *     dropped per Game-specific-content exclusion.
 */

# include <type.h>
# include <XML.h>

# define SID		"/usr/Schema/sys/schema_daemon"
# define SCHEMA_NODE	"/usr/Schema/obj/schema_node"

# define LPC_STR	"lpc_str"
# define LPC_OBJ	"lpc_obj"

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";

inherit "/usr/Schema/lib/dtd";

mapping namespaces;

private void configure_initial_nodes();

static void create()
{
    namespaces = ([ ]);
    set_object_name("Schema:Daemon");

    compile_object(SCHEMA_NODE);

    configure_initial_nodes();
}

int boot(int block)
{
    /* Core schemas are code-defined via configure_initial_nodes(); the
     * scaffolded load_core_schemas() reads data/schema/<file>.xml when
     * a stateimpex-equivalent loader lands at LV-4.5c. */
    return 60;
}

nomask void patch()
{
    set_object_name("Schema:Daemon");
}

# define Node(a,b) \
  ((tmp = find_object("Schema:" + (a) + ":" + (b))) ? \
   (tmp->clear_element(), tmp->set_name(a, b), tmp) : \
   (tmp = clone_object(SCHEMA_NODE), tmp->set_name(a, b), tmp))

/* this can be run if somebody edits a Schema:Schema:* element badly */

void patch_elements()
{
    configure_initial_nodes();
}

private void configure_initial_nodes()
{
    object tmp, element, children, attributes, iterator, callbacks;

    /* the structural primitives (Schema-for-schemas) */

    {
	object child;

	child = Node("Schema", "Child");

	child->add_attribute("node", LPC_OBJ);
	child->set_iterator("node", "query_children");
	child->add_callback("add_child", "node");

	children = Node("Schema", "Children");
	children->add_child(child);
	children->add_callback("clear_children");
    }

    {
	object attribute;

	attribute = Node("Schema", "Attribute");
	attribute->add_attribute("id", LPC_STR);
	attribute->set_iterator("id", "query_attributes");
	attribute->add_attribute("atype", LPC_STR,
				 "query_attribute_type", "id");
	attribute->add_attribute("acomment", LPC_STR,
				 "query_attribute_comment", "id");
	attribute->add_attribute("areadonly", XML_BOOL,
				 "query_attribute_readonly", "id");
	attribute->add_attribute("aquery", LPC_STR,
				 "make_attribute_query", "id");
	attribute->add_callback("add_made_attribute", "id", "atype", "aquery");
	attribute->add_callback("set_attribute_comment", "id", "acomment");
	attribute->add_callback("set_attribute_readonly", "id", "areadonly");

	attributes = Node("Schema", "Attributes");
	attributes->add_child(attribute);
	attributes->add_callback("clear_attributes");
    }

    {
	object callback;

	callback = Node("Schema", "Callback");
	callback->add_attribute("call", LPC_STR);
	callback->set_iterator("call", "make_callbacks");
	callback->add_callback("add_made_callback", "call");

	callbacks = Node("Schema", "Callbacks");
	callbacks->add_child(callback);
	callbacks->add_callback("clear_callbacks");
    }

    iterator = Node("Schema", "Iterator");
    iterator->add_attribute("var", LPC_STR, "query_iterator_variable");
    iterator->add_attribute("call", LPC_STR, "make_iterator_call");
    iterator->add_callback("set_made_iterator", "var", "call");

    element = Node("Schema", "Element");
    element->add_attribute("type", LPC_STR, "query_type");
    element->add_attribute("ns", LPC_STR, "query_namespace");
    element->add_attribute("tag", LPC_STR, "query_tag");
    element->add_attribute("recpoint", XML_BOOL, "query_recursion_point");
    element->add_attribute("transient", XML_BOOL, "query_transient");
    element->add_attribute("query", LPC_STR, "make_query");
    element->add_attribute("newitem", LPC_STR, "make_newitem");
    element->add_attribute("delitem", LPC_STR, "make_delitem");
    element->add_attribute("comment", LPC_STR, "query_comment");
    element->add_child(children);
    element->add_child(attributes);
    element->add_child(iterator);
    element->add_child(callbacks);
    element->add_callback("set_name", "ns", "tag");
    element->add_callback("set_recursion_point", "recpoint");
    element->add_callback("set_transient", "transient");
    element->add_callback("set_made_leaf", "type", "query");
    element->add_callback("set_made_newitem", "newitem");
    element->add_callback("set_made_delitem", "delitem");
    element->add_callback("set_comment", "comment");

    /* Ur:Hierarchy primitives (was Ur:UrObject / UrChild / UrChildren).
     * Note 4 rename applied: UrObject -> Hierarchy; the `urobject`
     * attribute renamed to `parent` so participating-object API surface
     * reads more naturally (query_ur_object stays as the LPC method
     * name for now, per Note 4's smaller-blast-radius decision). */

    {
	object hierarchy, urChild, urChildren;

	hierarchy = Node("Ur", "Hierarchy");
	hierarchy->add_attribute("parent", LPC_OBJ, "query_ur_object");
	hierarchy->add_callback("set_ur_object", "parent");

	urChild = Node("Ur", "Child");
	urChild->add_attribute("urchild", LPC_OBJ);
	urChild->set_iterator("urchild", "query_ur_children");

	urChildren = Node("Ur", "Children");
	urChildren->set_recursion_point(TRUE);
	urChildren->set_transient(TRUE);
	urChildren->add_child(urChild);
    }

    /* Core:Entry primitives (was Core:Property / Properties). Note 4
     * rename applied: Property -> Entry with attribute id `property` ->
     * `key`. The leaf type stays lpc_str; the marshaling callbacks stay
     * the SkotOS query/clear/set_ascii_property naming until a wider
     * property-API rename lands. */

    {
	object entry, entries;

	entry = Node("Core", "Entry");
	entry->set_leaf("lpc_str", "query_ascii_property", "key");
	entry->add_attribute("key", LPC_STR);
	entry->set_iterator("key", "query_property_indices");
	entry->add_callback("set_ascii_property", "key");

	entries = Node("Core", "Entries");
	entries->set_recursion_point(TRUE);
	entries->add_child(entry);
	entries->add_callback("clear_all_properties");
    }
}


/* schema-loading scaffolding (deferred to LV-4.5c stateimpex lift) */

private void load_core_schemas()
{
    /* When stateimpex lifts at LV-4.5c, this iterates the data/schema/
     * directory and dispatches each XML file through stateimpex's
     * import_state to instantiate a schema_node clone. For now the
     * core primitives are code-defined in configure_initial_nodes()
     * above and this function is a placeholder. The XML files in
     * data/schema/ carry the on-disk format reference. */
}


/* registry */

object query_node(string nm, varargs string extra)
{
    mapping map;
    string sp, t;

    if (extra != nil) {
	sp = nm; t = extra;
    } else if (!sscanf(nm, "%s:%s", sp, t)) {
	sp = "X"; t = nm;
    }
    if (map = namespaces[lower_case(sp)]) {
	return map[lower_case(t)];
    }
}

object get_node(string nm, varargs string extra)
{
    object ob;

    if (ob = query_node(nm, extra)) {
	return ob;
    }
    error("node " + nm + " unknown");
}

object get_root_node(object ob)
{
    string el;

    if (el = ob->query_state_root()) {
	return get_node(el);
    }
    error("no Schema root node defined for object");
}

void register(string sp, string t)
{
    mapping map;

    if (!sp || !t) {
	error("bad arguments");
    }
    sp = lower_case(sp);
    map = namespaces[sp];
    if (!map) {
	map = namespaces[sp] = ([ ]);
    }
    t = lower_case(t);
    if (map[t] && map[t] != previous_object()) {
	error("tag already exists");
    }
    map[t] = previous_object();
}

void deregister(string sp, string t)
{
    mapping map;

    if (map = namespaces[lower_case(sp)]) {
	map[lower_case(t)] = nil;
    }
}

string *query_namespaces()
{
    return map_indices(namespaces);
}

string *query_tags(string sp)
{
    mapping map;

    if (map = namespaces[lower_case(sp)]) {
	return map_indices(map);
    }
    return nil;
}
