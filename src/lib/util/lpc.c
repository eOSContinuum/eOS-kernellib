/*
 * LPC convenience helpers for code lifted from SkotOS.
 *
 * dumpValue: recursive stringification of any LPC value, matching the
 *            quoted-and-escaped shape used by SkotOS's dump_value and
 *            cloud-server's admin_console::dump_value. Useful in error
 *            messages and trace logs where the value's structure matters.
 *
 * member:    array contains predicate.
 *
 * sysLog, info, debugLog: logging stubs. Preserve call sites at lift time;
 *            pending a kernel-layer log facility, these are no-ops with
 *            TODO markers. Replace with concrete wiring (e.g., DRIVER
 *            message() or a kernel logger daemon) when one exists.
 */

# include <type.h>


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
 * logging stubs (no-op pending a kernel-layer log facility)
 *
 * Call sites preserved at lift time so SkotOS-shape trace structure
 * survives intact; concrete wiring (DRIVER message(), kernel logger
 * daemon, etc.) lands when a log story exists for the kernel layer.
 */

static void info(string msg)
{
    /* TODO: wire to log facility */
}

static void sysLog(string msg)
{
    /* TODO: wire to log facility */
}

static void debugLog(string msg)
{
    /* TODO: wire to log facility */
}
