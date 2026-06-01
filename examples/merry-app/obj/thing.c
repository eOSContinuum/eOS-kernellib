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
 * pattern surfaced on the LM-3.5 throwaway probe object.
 *
 * query_state_root() returns "MerryApp:Thing" so a future per-app
 * Schema registration can bind this clonable into the marshaler.
 * stateimpex round-trip is not exercised here -- LM-4's scope is the
 * runtime invocation path, not on-disk persistence. The application's
 * structure stays compatible with adding a Schema registration later.
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

static void create()
{
    properties::create();
    ur::create();
}
