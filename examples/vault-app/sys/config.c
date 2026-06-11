/*
 * Singleton configuration daemon.
 *
 * The Vault stores two XML shapes: <clone program="..."> for instances
 * of a clonable (obj/thing exercises that path) and <object program="...">
 * for one-of-a-kind daemons. This daemon exists to exercise the
 * <object> path end-to-end: the test driver stores it, destructs it,
 * and respawns it by name, which walks vault_node's find-or-load
 * branch (compile the program fresh) instead of the clone_object
 * branch.
 *
 * Deliberately NOT compiled by initd.c: the respawn assertion is only
 * meaningful when the program is not loaded, so the test driver
 * compiles it on demand and the Vault recompiles it during respawn.
 *
 * queryStateRoot() returns "MyApp:Config", the schema name the test
 * driver registers at boot. The schema declares `greeting` (lpc_str)
 * and `limit` (lpc_int) with the query_/set_ callback pairs below.
 */

# include <type.h>

inherit "/lib/util/named";

private string _greeting;
private int _limit;

string queryStateRoot()
{
    return "MyApp:Config";
}

string query_greeting() { return _greeting; }
int    query_limit()    { return _limit; }

void set_greeting(string val) { _greeting = val; }
void set_limit(int val)       { _limit = val; }
