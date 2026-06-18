/*
 * LPC convenience helpers shared across the lifted subsystems.
 *
 * dumpValue: recursive stringification of any LPC value, matching the
 *            quoted-and-escaped shape of admin_console::dump_value.
 *            Useful in error
 *            messages and trace logs where the value's structure matters.
 *
 * member:    array contains predicate.
 *
 * sysLog, info, debugLog: logging forwarders. Each forwards to the logd
 *            daemon (/usr/System/sys/logd) at a fixed severity
 *            (sysLog=NOTICE, info=INFO, debugLog=DEBUG); logd owns the
 *            threshold and the sink (see the forwarders below).
 */

# include <type.h>
# include <XML.h>
# include <log.h>


/*
 * recursive worker for dumpValue; tracks seen arrays/mappings for cycle
 * detection. Mirrors src/kernel/lib/admin_console.c dump_value() exactly,
 * so output format matches admin-console convention.
 */
private string _dumpValueR(mixed value, mapping seen)
{
    string str, *result;
    int i, sz;
    mixed *indices, *values;

    switch (typeof(value)) {
    case T_FLOAT:
	str = (string) value;
	if (sscanf(str, "%*s.") == 0 && sscanf(str, "%*se") == 0) {
	    if (value >= 0.0) {
		value += .05;
		str = (string) floor(value);
	    } else {
		value -= .05;
		str = (string) ceil(value);
	    }
	    str += "." + floor(fmod(fabs(value) * 10.0, 10.0));
	}
	return str;

    case T_INT:
	return (string) value;

    case T_STRING:
	str = value;
	if (sscanf(str, "%*s\\") != 0) {
	    str = implode(explode("\\" + str + "\\", "\\"), "\\\\");
	}
	if (sscanf(str, "%*s\"") != 0) {
	    str = implode(explode("\"" + str + "\"", "\""), "\\\"");
	}
	if (sscanf(str, "%*s\n") != 0) {
	    str = implode(explode("\n" + str + "\n", "\n"), "\\n");
	}
	if (sscanf(str, "%*s\t") != 0) {
	    str = implode(explode("\t" + str + "\t", "\t"), "\\t");
	}
	return "\"" + str + "\"";

    case T_OBJECT:
	return "<" + object_name(value) + ">";

    case T_ARRAY:
	if (seen[value]) {
	    return "#" + (seen[value] - 1);
	}
	seen[value] = map_sizeof(seen) + 1;

	sz = sizeof(value);
	result = allocate(sz);
	for (i = 0; i < sz; i++) {
	    result[i] = _dumpValueR(value[i], seen);
	}
	return "({ " + implode(result, ", ") + " })";

    case T_MAPPING:
	if (seen[value]) {
	    return "@" + (seen[value] - 1);
	}
	seen[value] = map_sizeof(seen) + 1;

	sz = map_sizeof(value);
	indices = map_indices(value);
	values = map_values(value);
	result = allocate(sz);
	for (i = 0; i < sz; i++) {
	    result[i] = _dumpValueR(indices[i], seen) + ":" +
			_dumpValueR(values[i], seen);
	}
	return "([ " + implode(result, ", ") + " ])";

    case T_NIL:
	return "nil";
    }
}

/*
 * return a recursive string describing any LPC value
 */
static string dumpValue(mixed value)
{
    return _dumpValueR(value, ([ ]));
}

/*
 * array contains: is item in arr?
 */
static int member(mixed item, mixed *arr)
{
    return sizeof(arr & ({ item })) > 0;
}

/*
 * return ob's logical name: query_object_name() if defined, else the LPC
 * object_name kfun. A richer fallback through query_parent for
 * unnamed clones with an ur parent doesn't arise for objects created
 * via vault_node.c's spawn_create_one (every clone gets set_object_name
 * at create time), so the two-step fallback suffices. Revise when an
 * unnamed-ur-clone case is discovered.
 */
static string name(object ob)
{
    string oname;

    oname = ob->query_object_name();
    return oname ? oname : object_name(ob);
}

/*
 * find an object by LPC path; compile_object if not present.
 */
static object findOrLoad(string path)
{
    object ob;

    ob = find_object(path);
    if (!ob) {
	ob = compile_object(path);
    }
    return ob;
}

/*
 * unwrap XML data wrapper objects to their underlying values. Used by
 * stateimpex callers (spawn_create_one, spawn_configure_one) to peek
 * inside parse_xml's typed-tree output.
 *
 * The XML element + pcdata + samref wrappers are recognized; samref is
 * a structural primitive (the LWO carries XML data with no semantic
 * interpretation here).
 */
static mixed queryColourValue(mixed value)
{
    if (typeof(value) == T_OBJECT) {
	switch (object_name(value)) {
	case "/usr/XML/data/element#-1":
	case "/usr/XML/data/pcdata#-1":
	case "/usr/XML/data/samref#-1":
	    return value->query_data();
	default:
	    break;
	}
    }
    return value;
}

/*
 * return the colour constant for an XML data wrapper LWO, or 0 if
 * value is not one of the recognized XML data wrappers. Used by
 * xmlgen.c::generate_xml to dispatch on element / pcdata / samref kind.
 */
static int queryColour(mixed value)
{
    if (typeof(value) == T_OBJECT) {
	switch (object_name(value)) {
	case "/usr/XML/data/element#-1":
	    return COL_ELEMENT;
	case "/usr/XML/data/pcdata#-1":
	    return COL_PCDATA;
	case "/usr/XML/data/samref#-1":
	    return COL_SAMREF;
	}
    }
    return 0;
}

/*
 * invert a mapping: swap each (key, value) pair so values become keys
 * and keys become values. Used by entities.c to build the reverse
 * entity-name lookup at module load time.
 */
static mapping reverseMapping(mapping m)
{
    mapping result;
    mixed *indices, *values;
    int i, sz;

    sz = map_sizeof(m);
    indices = map_indices(m);
    values = map_values(m);
    result = ([ ]);
    for (i = 0; i < sz; i++) {
	result[values[i]] = indices[i];
    }
    return result;
}

/*
 * logging forwarders. sysLog / info / debugLog forward to the logd daemon
 * (/usr/System/sys/logd) at fixed severity levels; logd applies the
 * threshold and owns the sink. The find_object guard drops the message if
 * logd is absent (the boot edge before System init loads it, or a recompile
 * window) so a log call never breaks the caller.
 */

static void info(string msg)
{
    object logd;

    if ((logd=find_object(LOGD))) {
	logd->log(LOG_INFO, msg);
    }
}

static void sysLog(string msg)
{
    object logd;

    if ((logd=find_object(LOGD))) {
	logd->log(LOG_NOTICE, msg);
    }
}

static void debugLog(string msg)
{
    object logd;

    if ((logd=find_object(LOGD))) {
	logd->log(LOG_DEBUG, msg);
    }
}
