/*
 * Property + ur-bearing demonstration clonable.
 *
 * Composes the three caller-side contracts that Merry's find_merry /
 * run_merry / find_merries surface walks against:
 *
 *   - /lib/util/named       set_object_name + Index-backed find_named
 *   - /lib/util/properties  set_property / query_raw_property /
 *                           query_prefixed_properties (the storage
 *                           Merry's find_merry walks via the
 *                           merry:<mode>:<signal> key convention)
 *   - /lib/util/ur          set_ur_object / query_ur_object
 *                           (the ancestry walk that find_merry follows
 *                           to look up scripts inherited from an
 *                           ur-parent)
 *
 * The properties and ur libs both define static void create(); cloud-
 * server's inherit resolution requires labels to disambiguate. Same
 * pattern recurs for any pair of libs that both define create().
 *
 * query_state_root() returns "MerryApp:Thing"; sys/test registers the
 * matching schema at boot (one lpc_str attribute, `label`, backed by
 * the property store). The Duplicate merryfun walks this state model
 * when it copies a clone's state.
 *
 * The $delay() continuation glue Merry requires of every
 * script-bearing object lives in /lib/util/delayed, inherited below.
 */

# include <type.h>

inherit "/lib/util/named";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";
inherit "/lib/util/delayed";

string query_state_root() { return "MerryApp:Thing"; }

/* Typed accessors for the MerryApp:Thing schema (registered by
 * sys/test at boot). Backed by the property store so the marshaled
 * state and the runtime state are the same value. */
string query_label() { return query_property("demo:label"); }
void set_label(string val) { set_property("demo:label", val); }

static void create()
{
    properties::create();
    ur::create();
}
