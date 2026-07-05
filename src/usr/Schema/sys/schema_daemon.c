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
 * Core schemas are code-defined in configure_initial_nodes() --
 * structural primitives plus the Ur:Hierarchy / Ur:Child / Ur:Children
 * ancestry shapes and the Core:Entry / Core:Entries property-bag
 * shapes. XML-file-driven loading (load_core_schemas) cross-checks the
 * code-defined primitives against the data/schema/ files. Vault
 * participation lives in the Vault subsystem; the marshaler
 * (~Marshal/XmlBinding/lib/stateimpex) coordinates the vault path.
 * The type-handler dispatch API is camelCase across
 * its callers.
 */

# include <type.h>
# include <XML.h>

# define SID		"/usr/Schema/sys/schema_daemon"
# define SCHEMA_NODE	"/usr/Schema/obj/schema_node"

# define LPC_STR	"lpc_str"
# define LPC_OBJ	"lpc_obj"

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";

private inherit "~XML/lib/xmd";

inherit "/lib/util/named";
inherit "/usr/Schema/lib/dtd";

/* The stateimpex inherit lets load_core_schemas() call import_state()
 * directly to dispatch parsed XML schema files through the marshaler. */
inherit "~Marshal/XmlBinding/lib/stateimpex";

mapping namespaces;

private void configure_initial_nodes();
object query_node(string nm, varargs string extra);

static void create()
{
    namespaces = ([ ]);
    set_object_name("Schema:Daemon");

    compile_object(SCHEMA_NODE);

    configure_initial_nodes();

    /* Defer load_core_schemas() until the next driver tick so that
     * /usr/XML/sys/xml_daemon is loaded and available for parse_xml.
     * System/initd loads domains alphabetically: Marshal < Schema < XML,
     * so XML's initd runs after Schema's create() returns. call_out 0
     * fires when System/initd::create() has finished the load loop. */
    call_out("load_core_schemas", 0);
}

int boot(int block)
{
    /* Core schemas are code-defined via configure_initial_nodes(); the
     * scaffolded load_core_schemas() reads data/schema/<file>.xml when
     * the marshaler loads them. */
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
     * reads more naturally (query_parent stays as the LPC method
     * name for now, per Note 4's smaller-blast-radius decision). */

    {
	object hierarchy, urChild, urChildren;

	hierarchy = Node("Ur", "Hierarchy");
	hierarchy->add_attribute("parent", LPC_OBJ, "query_parent");
	hierarchy->add_callback("set_ur_object", "parent");

	urChild = Node("Ur", "Child");
	urChild->add_attribute("urchild", LPC_OBJ);
	urChild->set_iterator("urchild", "query_ur_children");

	urChildren = Node("Ur", "Children");
	urChildren->set_recursion_point(TRUE);
	urChildren->set_transient(TRUE);
	urChildren->add_child(urChild);
    }

    /* Core:Entry primitives -- the property-table marshaling shape for
     * bare property-bearing objects. The accessors live in /lib/util/
     * properties over the /lib/util/coercion codec; the leaf type
     * stays lpc_str (the accessor emits the encoded string itself),
     * and the query/clear/set_ascii_property naming stays until a
     * wider property-API rename lands. This definition and
     * data/schema/Entry.xml declare the same shape; keep them in
     * step. */

    {
	object entry, entries;

	entry = Node("Core", "Entry");
	entry->set_leaf("lpc_str", "query_ascii_property", "key");
	entry->add_attribute("key", LPC_STR);
	entry->set_iterator("key", "query_property_indices", "#0");
	entry->set_delitem("clear_property", "key");
	entry->add_callback("set_ascii_property", "key", "CONTENT");

	entries = Node("Core", "Entries");
	entries->set_recursion_point(TRUE);
	entries->add_child(entry);
	entries->add_callback("clear_all_properties");
    }
}


/* schema-loading: re-applies the XML schema files in data/schema/ over
 * the code-defined primitives. Each file declares <object
 * program="schema_node"> wrapping a single <Element ns="..." tag="...">
 * with the same structure that configure_initial_nodes() produces in
 * code. Loading via the lifted stateimpex round-trips the file through
 * schema_daemon's own structural primitives -- the Element / Children /
 * Attributes / Iterator / Callbacks schema-for-schemas tree. The XML
 * shape WINS: the import callbacks clear and re-add list-valued
 * definitions (attributes, callbacks) and the made-setters replace the
 * leaf, iterator, and delitem, so a file that diverges from the code
 * definition silently replaces it rather than erroring -- keep the two
 * in step by hand. Parse and structural failures do surface as load
 * errors in boot.log. import_state is idempotent on a matching node:
 * re-applying the same state produces the same state. Mismatches surface
 * as load errors in boot.log. */

void load_core_schemas()
{
    string *files;
    int i;

    if (!find_object(XML)) {
	sysLog("Schema:Daemon: XML daemon not loaded; skipping load_core_schemas");
	return;
    }

    files = get_dir("/usr/Schema/data/schema/*.xml")[0];
    for (i = 0; i < sizeof(files); i++) {
	string path, source, ns, tag;
	mixed root, element, *attrs;
	object node;
	int j;

	path = "/usr/Schema/data/schema/" + files[i];
	source = read_file(path);
	if (!source) {
	    sysLog("Schema:Daemon: cannot read " + path);
	    continue;
	}

	catch {
	    root = XML->parse(source);
	} : {
	    sysLog("Schema:Daemon: parse failed for " + path);
	    continue;
	}

	root = queryColourValue(xmdForceToData(root));
	if (sizeof(root) == 0 || xmdElement(root[0]) != "object") {
	    sysLog("Schema:Daemon: " + path + " root is not <object>");
	    continue;
	}

	element = queryColourValue(xmdForceToData(xmdContent(root[0])));
	if (sizeof(element) == 0) {
	    sysLog("Schema:Daemon: " + path + " <object> has no content");
	    continue;
	}
	element = element[0];

	attrs = xmdAttributes(element);
	ns = tag = nil;
	for (j = 0; j < sizeof(attrs); j += 2) {
	    if (attrs[j] == "ns")  ns  = attrs[j+1];
	    if (attrs[j] == "tag") tag = attrs[j+1];
	}
	if (!ns || !tag) {
	    sysLog("Schema:Daemon: " + path + " <" + xmdElement(element)
		   + "> missing ns/tag attributes");
	    continue;
	}

	node = query_node(ns, tag);
	if (!node) {
	    sysLog("Schema:Daemon: " + path + " no code-defined node for "
		   + ns + ":" + tag + " (cannot cross-check)");
	    continue;
	}

	catch {
	    import_state(node, element);
	    sysLog("Schema:Daemon: cross-checked " + ns + ":" + tag
		   + " from " + files[i]);
	} : {
	    sysLog("Schema:Daemon: import_state FAILED for " + ns + ":"
		   + tag + " from " + files[i]);
	}
    }
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

    if (el = ob->queryStateRoot()) {
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
