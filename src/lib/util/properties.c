/*
 * Generic property access mechanism.
 *
 * The ascii-property accessor pair (query_ascii_property /
 * set_ascii_property) marshals property values through the
 * /lib/util/coercion codec, so the Core:Entry schema callbacks that
 * reference the pair give every bare property-bearing object a
 * marshaling path over its property table -- no per-app schema
 * required for simple values.
 *
 * Inheritors receive queryStateRoot() returning "Core:Entries" by
 * default (the property-table marshaling shape); applications whose
 * durable state lives in typed member variables instead bind their own
 * schema (the vault-app/obj/thing.c pattern: queryStateRoot() ->
 * "MyApp:Thing").
 */

# include <type.h>

private inherit "/lib/util/ascii";
private inherit "/lib/util/coercion";

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


string queryStateRoot() { return "Core:Entries"; }


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
 * avoid re-entry). The writer's program is captured here and threaded to
 * dispatch_set so the dispatcher can apply the registrar capability to
 * direct writes of observer-registration (merry:on:*) properties.
 */
nomask
mixed set_property(string prop, mixed val, varargs mixed extra...)
{
    object merry;
    string caller;

    caller = previous_program();
    prop = lower_case(prop);
    merry = find_object(MERRY_DAEMON);
    if (merry) {
	return merry->dispatch_set(this_object(), prop, val, caller);
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

/*
 * Ascii-property accessors: the Core:Entry schema's marshaling
 * surface. The getter encodes the stored value through the coercion
 * codec; the setter decodes and writes through set_property, so
 * observers fire and the dispatch-path write gate applies to imported
 * writes exactly as to any other write.
 */
string query_ascii_property(string prop)
{
    return encodeValue(query_property(prop));
}

void set_ascii_property(string prop, string ascii)
{
    set_property(prop, decodeValue(ascii));
}

/*
 * The property mapping. By default the reserved merry: namespace
 * (observer registrations, scripts, dispatcher bookkeeping) is
 * excluded: those entries are runtime wiring re-established through
 * the gated registration surfaces, not marshalable application state.
 * Pass a non-zero argument to include them.
 */
mapping query_properties(varargs int opaque)
{
    mapping map;

    map = properties[..];
    if (!opaque) {
	map -= map_indices(prefixed_map(map, "merry:"));
    }
    return map;
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
