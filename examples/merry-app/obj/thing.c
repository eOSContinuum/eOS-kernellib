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
 * delayed_call / perform_delayed_call below are required by Merry's
 * $delay() statement. merrynode.c::do_delay invokes
 * `::call_other(this, "delayed_call", mcontext_LWO, "merry_continuation",
 * delay, this)`, so every script-bearing object (the type passed as
 * `this` at run_merry / run_merries) must expose this pair. The shape
 * is lifted from SkotOS /core/lib/core_scripts.c lines 47-54; this
 * inline copy keeps the demonstration self-contained. Promote to a
 * /lib/util helper if a second application surfaces the same need.
 */

# include <type.h>

inherit "/lib/util/named";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";

string query_state_root() { return "MerryApp:Thing"; }

static void create()
{
    properties::create();
    ur::create();
}

static void perform_delayed_call(object ob, string fun, mixed *args)
{
    call_other(ob, fun, args...);
}

void delayed_call(object ob, string fun, mixed delay, mixed args...)
{
    call_out("perform_delayed_call", delay, ob, fun, args);
}
