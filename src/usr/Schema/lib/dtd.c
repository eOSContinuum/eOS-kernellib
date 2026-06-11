/*
 * DTD inheritable: poor-man's abstract data type repository.
 *
 * Inherited by callers that need type-system queries (xmlgen / xmlparse /
 * xml_daemon / schema_daemon). Delegates everything to the dtd_daemon's
 * registered type and colour handlers.
 *
 * ascii_to_untyped parses only the bare ASCII fallthrough (no
 * typed-literal forms).
 */

# include <type.h>

# define DTD		"/usr/Schema/sys/dtd_daemon"

private inherit "/lib/util/lpc";

static int query_type_colour(string type)
{
    return DTD->get_type_handler(type)->query_type_colour(type);
}

static string query_colour_type(int colour)
{
    return DTD->get_colour_handler(colour)->query_colour_type(colour);
}

static string *query_ascii_enumeration(string type)
{
    return DTD->query_enumeration(type);
}

static int query_checkboxed(string type)
{
    return DTD->get_type_handler(type)->query_checkboxed(type, ([ ]));
}

static int test_raw_data(mixed val, string type)
{
    return DTD->get_type_handler(type)->test_raw_data(val, type);
}

static mixed default_value(string type)
{
    return DTD->get_type_handler(type)->default_value(type);
}

static string typed_to_ascii(mixed val, string type);

static string untyped_to_ascii(mixed val)
{
    string type;
    int colour;

    if (colour = queryColour(val)) {
	if (type = query_colour_type(colour)) {
	    return typed_to_ascii(val, type);
	}
    }
    switch (typeof(val)) {
    case T_STRING:
	return val;
    case T_INT: case T_FLOAT:
	return val + "";
    case T_OBJECT:
	return "OBJ(" + name(val) + ")";
    case T_ARRAY:
	return "ARR[" + sizeof(val) + "]";
    case T_MAPPING:
	return "MAP[" + map_sizeof(val) + "]";
    case T_NIL:
	return "[NIL]";
    }
    error("eek");
}

static string typed_to_ascii(mixed val, string type)
{
    if (!type) {
	return untyped_to_ascii(val);
    }
    return DTD->get_type_handler(type)->typed_to_ascii(val, type);
}

static mixed ascii_to_untyped(string ascii)
{
    string o;
    object ob;

    if (sscanf(ascii, "OBJ(%s)", o)) {
	/* path lookup first (file-backed OBJ refs), then Index logical-name
	 * lookup for OBJ(Schema:Foo:Bar)-style literals in lifted schema
	 * XML (closes OQ-17). */
	if (ob = find_object(o)) {
	    return ob;
	}
	if (ob = "/usr/Index/sys/index_daemon"->query_object(o)) {
	    return ob;
	}
	error("no object: " + o);
    }
    return ascii;
}


static mixed ascii_to_typed(string ascii, string type)
{
    if (!type) {
	return ascii_to_untyped(ascii);
    }
    return DTD->get_type_handler(type)->ascii_to_typed(ascii, type);
}

static int ascii_size(string type)
{
    return DTD->get_type_handler(type)->query_asciisize(type);
}

static int ascii_height(string type)
{
    return DTD->get_type_handler(type)->query_asciiheight(type);
}
