/*
 * Generic property access mechanism.
 *
 * The ascii-property accessor pair (query_ascii_property /
 * set_ascii_property) that the Core:Entry schema callbacks reference is
 * not provided yet -- it depends on a richer type-coercion layer; until
 * it lands, per-app schemas with typed accessors are the marshaling
 * path for property-bearing objects.
 *
 * Inheritors receive queryStateRoot() returning "Core:Properties" by default;
 * application objects that bind to their own schema must override (the
 * vault-app/obj/thing.c pattern: queryStateRoot() -> "MyApp:Thing").
 */

# include <type.h>

private inherit "/lib/util/ascii";

private mapping properties;

/*
 * MERRY dispatcher hook. set_property routes through MERRY->dispatch_set
 * when the daemon is loaded so observers (dispatcher registrations) fire around
 * the actual write. set_raw_property is the bypass path used by the
 * dispatcher itself to perform the write step without re-entering
 * dispatch; it is also available to callers that need to skip dispatch
 * for performance-sensitive paths.
 */
# define MERRY_DAEMON	"/usr/Merry/sys/merry"


/*
 * Sub-mapping with keys starting with `prefix` (and lexicographically
 * up to prefix + "\377") (only the prefix-window mode
 * is provided; Merry's call sites need nothing more).
 */
private mapping prefixed_map(mapping map, string prefix)
{
    return map[prefix .. prefix + "\377"];
}


string queryStateRoot() { return "Core:Properties"; }


static
void create()
{
    properties = ([ ]);
}


void clear_downcased_property(string prop)
{
    properties[prop] = nil;
}

nomask
void clear_property(string prop)
{
    clear_downcased_property(lower_case(prop));
}

void clear_all_properties()
{
    properties = ([ ]);
}

mixed set_downcased_property(string prop, mixed val, varargs mixed extra...)
{
    properties[prop] = val;
}

/*
 * Bypass dispatch and write directly. The dispatcher itself uses this
 * to perform the actual write between pre and main observer phases;
 * callers that need to skip dispatch (capability-gated infrastructure,
 * performance-sensitive paths) may also use it.
 */
nomask
void set_raw_property(string prop, mixed val)
{
    properties[lower_case(prop)] = val;
}

/*
 * Public property write. Routes through MERRY->dispatch_set when the
 * daemon is loaded so observers fire pre/main/post around the write;
 * falls back to direct write before MERRY is compiled (early bootstrap)
 * and inside MERRY's own dispatcher (which calls set_raw_property to
 * avoid re-entry).
 */
nomask
mixed set_property(string prop, mixed val, varargs mixed extra...)
{
    object merry;

    prop = lower_case(prop);
    merry = find_object(MERRY_DAEMON);
    if (merry) {
	return merry->dispatch_set(this_object(), prop, val);
    }
    return set_downcased_property(prop, val, extra...);
}

mixed query_downcased_property(string prop)
{
    return properties[prop];
}

/* For Merry script lookups: returns whatever is stored under the exact
 * key, bypassing the lower_case() pass that query_property applies. */
nomask
mixed query_raw_property(string prop)
{
    return properties[prop];
}

/* For Merry script lookups: collect all stored keys with a given
 * prefix (e.g., "merry:act:foo%" matches "merry:act:foo",
 * "merry:act:foo-pre", etc.). */
nomask
mapping query_prefixed_properties(string prefix)
{
    return prefixed_map(properties, prefix);
}

nomask
mixed query_property(string prop)
{
    return query_downcased_property(lower_case(prop));
}

mapping query_properties(varargs int derived)
{
    return properties[..];
}

void add_properties(mapping map)
{
    properties += map;
}

string *query_property_indices(varargs int opaque)
{
    return map_indices(query_properties(opaque));
}

mapping query_intrinsic_properties()
{
    return properties[..];
}
