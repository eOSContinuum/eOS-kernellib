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

static int queryTypeColour(string type)
{
    return DTD->get_type_handler(type)->queryTypeColour(type);
}

static string queryColourType(int colour)
{
    return DTD->get_colour_handler(colour)->queryColourType(colour);
}

static string *query_ascii_enumeration(string type)
{
    return DTD->query_enumeration(type);
}

static int queryCheckboxed(string type)
{
    return DTD->get_type_handler(type)->queryCheckboxed(type, ([ ]));
}

static int testRawData(mixed val, string type)
{
    return DTD->get_type_handler(type)->testRawData(val, type);
}

static mixed defaultValue(string type)
{
    return DTD->get_type_handler(type)->defaultValue(type);
}

static string typedToAscii(mixed val, string type);

static string untyped_to_ascii(mixed val)
{
    string type;
    int colour;

    if (colour = queryColour(val)) {
	if (type = queryColourType(colour)) {
	    return typedToAscii(val, type);
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

static string typedToAscii(mixed val, string type)
{
    if (!type) {
	return untyped_to_ascii(val);
    }
    return DTD->get_type_handler(type)->typedToAscii(val, type);
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


static mixed asciiToTyped(string ascii, string type)
{
    if (!type) {
	return ascii_to_untyped(ascii);
    }
    return DTD->get_type_handler(type)->asciiToTyped(ascii, type);
}

static int ascii_size(string type)
{
    return DTD->get_type_handler(type)->queryAsciiSize(type);
}

static int ascii_height(string type)
{
    return DTD->get_type_handler(type)->queryAsciiHeight(type);
}
