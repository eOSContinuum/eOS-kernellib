/*
 * Property-bearing demonstration clonable.
 *
 * One string-typed `label` and one int-typed `count`. Implements the
 * Schema callback surface (query_/set_ pairs per attribute) that the
 * MyApp:Thing schema declares, plus the /lib/util/named API that
 * Vault->store / Vault->spawn_one_by_name use to round-trip the clone
 * by logical name.
 *
 * query_state_root() returns "MyApp:Thing", the schema name registered
 * by sys/test::create at boot. stateimpex::export_state(this_object())
 * looks up the schema_node by that name, then walks its attributes
 * calling query_label / query_count to assemble the XML body. Import
 * walks the same attributes in reverse and dispatches set_label /
 * set_count.
 */

# include <type.h>

inherit "/lib/util/named";

private string _label;
private int _count;

static void create()
{
    /* clones only -- master is a template, no per-master state. */
}

string query_state_root()
{
    return "MyApp:Thing";
}

string query_label()  { return _label; }
int    query_count()  { return _count; }

void set_label(string val) { _label = val; }
void set_count(int val)    { _count = val; }
