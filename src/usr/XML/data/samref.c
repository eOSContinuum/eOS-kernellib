/*
 * LWO for SAM-reference data.
 *
 * Lightweight wrapper preserved as a structural primitive at LV-4.5a:
 * SAM-side game-content interpretation is excluded by the Game-specific-content
 * exclusion, but the LWO carries XML data with no semantic interpretation here
 * — removing it would force xmd.c / xmlparse.c rewrite. xmlgen.c emits a
 * literal $(ref attrs) form for any reader that wants it; no game-content
 * rewrite happens in the lifted XML subsystem.
 */

# include <std.h>

mixed data;

int is_samref() { return TRUE; }

/* LV-4.5d: See element.c for the create-vs-configure rationale. */
static void create(mixed d)
{
    data = d;
}

void set_data(mixed d)
{
    data = d;
}

mixed query_data()
{
    return data;
}
