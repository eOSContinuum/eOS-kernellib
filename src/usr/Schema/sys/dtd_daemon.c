/*
 * DTD daemon: registry of data type definitions and their handlers.
 *
 * Handlers register themselves at create() / patch() time via
 * register_type(type) / register_colour(colour); the daemon dispatches
 * type-system queries (ascii<->typed conversion, validity checks, etc.)
 * to the registered handler.
 *
 * LPC_MIXED ascii conversion uses dumpValue for serialization and a
 * string-only round-trip for deserialization. This stays deliberately
 * degraded even though /lib/util/coercion now provides a round-trip
 * codec (the ascii-property accessors consume it): undeclared XML
 * attributes default to lpc_mixed, so its string-passthrough decode is
 * load-bearing for generic XML -- a round-tripping DTD type would be a
 * new type name, not a semantics change here. typed_to_html is a
 * nil-returning passthrough (HTML output is an admin-surface concern;
 * XML transport is the only public serialization). The type-handler
 * API is camelCase (queryTypeColour, typedToAscii, asciiToTyped,
 * testRawData, and kin); every registered handler implements the
 * same names, since dispatch is by name.
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

# define INDEX			"/usr/Index/sys/index_daemon"

mapping enumerations;
mapping type_handlers;
mapping colour_handlers;

string queryStateRoot() { return "DTD:Enumerations"; }

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


int queryTypeColour(string type)
{
    return 0;
}

int testRawData(mixed value, string type)
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

mixed defaultValue(string type)
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

string typedToAscii(mixed value, string type)
{
    if (!type) {
	return ::untyped_to_ascii(value);
    }
    switch (type) {
    case LPC_MIXED:
	/* Degraded mixed serialization via dumpValue, kept: lpc_mixed
	 * is the undeclared-attribute default, so its semantics stay
	 * string-shaped. Round-trip marshaling is the /lib/util/
	 * coercion codec's job (the ascii-property accessors). */
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

mixed asciiToTyped(string ascii, string type)
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
	/* Degraded mixed deserialization: the ASCII passes through as
	 * a string -- the undeclared-attribute contract. Typed
	 * round-trip decoding is the /lib/util/coercion codec's job
	 * (the ascii-property accessors). */
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
	    /* path lookup first (legacy / file-backed OBJ refs), then
	     * logical-name lookup via Index for OBJ(Schema:Foo:Bar)-style
	     * literals carried in lifted XML schemas. */
	    ob = find_object(str);
	    if (!ob) {
		ob = INDEX->query_object(str);
	    }
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

int queryAsciiSize(string type, varargs mapping args)
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

int queryAsciiHeight(string type, varargs mapping args)
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
