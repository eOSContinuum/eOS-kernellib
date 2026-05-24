/*
 * DTD daemon: registry of data type definitions and their handlers.
 *
 * Handlers register themselves at create() / patch() time via
 * register_type(type) / register_colour(colour); the daemon dispatches
 * type-system queries (ascii<->typed conversion, validity checks, etc.)
 * to the registered handler.
 *
 * Lifted from skoot/usr/DTD/sys/dtd.c. LV-4.5b refactors:
 * (a) <SAM.h> SkotOS include + SAM module + SAMD->register_root call
 *     stripped per Game-specific-content exclusion. SAM_loaded() hook
 *     dropped (it was a sugar-tag registration callback).
 * (b) /lib/module inherit dropped (SAM-module machinery, Game content).
 * (c) /lib/url inherit dropped + typed_to_html implementation stripped
 *     (HTML-output is an admin-surface concern; the lifted XML transport
 *     is the only public serialization). The typed_to_html stub remains
 *     as nil-returning passthrough for inherit-chain compile parity.
 * (d) /lib/mapargs inherit + mixed_to_ascii / ascii_to_mixed calls
 *     stripped; LPC_MIXED ascii conversion temporarily uses dumpValue
 *     for serialization and a string-only round-trip for deserialization
 *     pending a richer mapargs-style lift at a future task. This is a
 *     deliberate degradation noted in scope.md (future enhancement).
 * (e) /lib/string inherit replaced by /lib/util/ascii (lower_case).
 * (f) eval_sam_ref() dropped (SAM-side sugar-tag reference dispatch).
 * (g) Function names for the type-handler API kept snake_case as a
 *     contract surface (callers in xml_daemon.c / schema_node.c / future
 *     stateimpex inherit through /usr/Schema/lib/dtd and expect these
 *     names). LV-2.5b camelCase rename held at LV-4.5b lift time pending
 *     a coordinated sweep of the schema-API surface across all callers.
 * (h) name() / dumpValue helpers from /lib/util/lpc replace SkotOS
 *     `name(val)` / `dump_value(val)` bare calls.
 */

# include <type.h>

inherit "/usr/XML/lib/entities";

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";

private inherit "/usr/Schema/lib/dtd";

# define LPC_MIXED		"lpc_mixed"
# define LPC_STR		"lpc_str"
# define LPC_OBJ		"lpc_obj"
# define LPC_INT		"lpc_int"
# define LPC_FLT		"lpc_flt"

mapping enumerations;
mapping type_handlers;
mapping colour_handlers;

string query_state_root() { return "DTD:Enumerations"; }

static void create()
{
    type_handlers = ([
	LPC_MIXED: this_object(),
	LPC_STR:   this_object(),
	LPC_INT:   this_object(),
	LPC_FLT:   this_object(),
	LPC_OBJ:   this_object(),
    ]);
    colour_handlers = ([ ]);
    enumerations = ([ ]);
}

void patch()
{
    type_handlers = ([
	LPC_MIXED: this_object(),
	LPC_STR:   this_object(),
	LPC_INT:   this_object(),
	LPC_FLT:   this_object(),
	LPC_OBJ:   this_object(),
    ]);
    colour_handlers = ([ ]);
}

/* handler API */

void register_type(string type)
{
    sysLog("DTD:: Registered type '" + type + "' to " +
	   name(previous_object()));
    type_handlers[type] = previous_object();
}

object query_type_handler(string type)
{
    return type_handlers[type];
}

object get_type_handler(string type)
{
    if (type_handlers[type]) {
	return type_handlers[type];
    }
    error("no handler for type: " + dumpValue(type));
}


void register_colour(int colour)
{
    sysLog("DTD:: Registered colour '" + colour + "' to " +
	   name(previous_object()));
    colour_handlers[colour] = previous_object();
}

object query_colour_handler(int colour)
{
    return colour_handlers[colour];
}

object get_colour_handler(int colour)
{
    if (colour_handlers[colour]) {
	return colour_handlers[colour];
    }
    error("no handler for colour: " + colour);
}


/* enumerations */

string *query_enumerations()
{
    return map_indices(enumerations);
}

void clear_enumeration(string enum)
{
    enumerations[enum] = nil;
}

void extend_enumeration(string enum, string item)
{
    if (!enumerations[enum]) {
	enumerations[enum] = ([ ]);
    }
    enumerations[enum][item] = TRUE;
}

string *query_enumeration(string enum, varargs mapping args)
{
    mapping map;

    if (map = enumerations[enum]) {
	return map_indices(map);
    }
    return nil;
}

/* we handle queries on the raw LPC types ourselves */


int query_type_colour(string type)
{
    return 0;
}

int test_raw_data(mixed value, string type)
{
    switch (type) {
    case LPC_MIXED:
	return TRUE;
    case LPC_STR:
	return typeof(value) == T_STRING || typeof(value) == T_NIL;
    case LPC_OBJ:
	return typeof(value) == T_OBJECT || typeof(value) == T_NIL;
    case LPC_FLT:
	return typeof(value) == T_FLOAT;
    case LPC_INT:
	return typeof(value) == T_INT;
    }
    error("unknown type: " + type);
}

mixed default_value(string type)
{
    switch (type) {
    case LPC_MIXED:
    case LPC_STR:
    case LPC_OBJ:
	return nil;
    case LPC_FLT:
	return 0.0;
    case LPC_INT:
	return 0;
    }
    error("unknown type: " + type);
}

string typed_to_ascii(mixed value, string type)
{
    if (!type) {
	return ::untyped_to_ascii(value);
    }
    switch (type) {
    case LPC_MIXED:
	/* Degraded vs SkotOS mapargs-shape mixed_to_ascii: serialize via
	 * dumpValue. Round-trip not exact for arrays / mappings. Future
	 * task: lift a mapargs-equivalent helper into /lib/util. */
	return dumpValue(value);
    case LPC_STR:
	if (value == nil) {
	    return "";
	}
	if (typeof(value) == T_STRING) {
	    return value;
	}
	error("not a string");
    case LPC_OBJ:
	if (typeof(value) == T_OBJECT) {
	    return "OBJ(" + name(value) + ")";
	}
	if (typeof(value) == T_NIL) {
	    return "";
	}
	error("not an object");
    case LPC_INT: case LPC_FLT:
	return (string) value;
    }
    error("unknown type: " + type);
}

string typed_to_html(mixed value, string type)
{
    /* HTML output is admin-surface; the lifted XML transport is the
     * only serialization. Stub returns nil; callers tolerate it. */
    return nil;
}

mixed ascii_to_typed(string ascii, string type)
{
    string str;
    object ob;
    int n;
    float f;

    if (!type) {
	type = LPC_MIXED;
    }
    switch (type) {
    case LPC_MIXED:
	/* Degraded vs SkotOS ascii_to_mixed: only string round-trip is
	 * supported. Complex types (array, mapping) are not round-tripped
	 * back to LPC values; the caller receives the ASCII as a string.
	 * Future task: lift a mapargs-equivalent parser. */
	return ascii;
    case LPC_STR:
	if (!strlen(ascii)) {
	    return nil;
	}
	return ascii;
    case LPC_OBJ:
	if (!ascii || !strlen(ascii)) {
	    return nil;
	}
	if (sscanf(ascii, "OBJ(%s)", str)) {
	    ob = find_object(str);
	    if (!ob) {
		error("no object: " + ascii);
	    }
	    return ob;
	}
	error("value is not an object");
    case LPC_INT:
	if (sscanf(ascii, "%d", n)) {
	    return n;
	}
	error("value is not an integer");
    case LPC_FLT:
	if (sscanf(ascii, "%f", f)) {
	    return f;
	}
	error("value is not an float");
    }
    error("unknown type: " + type);
}

int query_asciisize(string type, varargs mapping args)
{
    switch (type) {
    case LPC_FLT:
	return 10;
    case LPC_INT:
	return 12;
    case LPC_STR:
    case LPC_OBJ:
    case LPC_MIXED:
	return 30;
    }
    error("unknown type: " + type);
}

int query_asciiheight(string type, varargs mapping args)
{
    switch (type) {
    case LPC_STR:
    case LPC_MIXED:
	return 1;
    case LPC_FLT:
    case LPC_INT:
    case LPC_OBJ:
	return 1;
    }
    error("unknown type: " + type);
}
