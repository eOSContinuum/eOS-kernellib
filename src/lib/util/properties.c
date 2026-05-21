/*
 * Generic property access mechanism.
 *
 * Lifted from SkotOS /lib/properties.c per LM-3.5. Strips at lift:
 *   inherit "/lib/womble"           -> dropped (SAM-related; LM-2 (c) dropped SAM)
 *   womble_properties()             -> dropped (same)
 *   query_ascii_property            -> dropped (depends on mixed_to_ascii not lifted)
 *   set_ascii_property              -> dropped (same)
 *   nullify static                  -> dropped (only used by ascii path)
 *   inherit "/lib/string"           -> inherit "/lib/util/ascii" (lower_case)
 *   inherit "/lib/mapping"          -> inline prefixed_map below
 *
 * Inheritors receive query_state_root() returning "Core:Properties" by default;
 * application objects that bind to their own schema must override (the
 * vault-app/obj/thing.c pattern: query_state_root() -> "MyApp:Thing").
 */

# include <type.h>

private inherit "/lib/util/ascii";

private mapping properties;


/*
 * Sub-mapping with keys starting with `prefix` (and lexicographically
 * up to prefix + "\377"). Lifted inline from SkotOS /lib/mapping.c per
 * LM-3.5 (the `prune` / `reverse` modes are unused by Merry's call
 * sites and dropped).
 */
private mapping prefixed_map(mapping map, string prefix)
{
    return map[prefix .. prefix + "\377"];
}


string query_state_root() { return "Core:Properties"; }


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

nomask
mixed set_property(string prop, mixed val, varargs mixed extra...)
{
    return set_downcased_property(lower_case(prop), val, extra...);
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
