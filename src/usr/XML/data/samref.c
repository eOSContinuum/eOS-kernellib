/*
 * LWO for SAM-reference data.
 *
 * Lightweight wrapper preserved as a structural primitive: the LWO
 * carries XML data with no semantic interpretation here -- removing it
 * would force an xmd.c / xmlparse.c rewrite. xmlgen.c emits a literal
 * $(ref attrs) form for any reader that wants it.
 */

# include <std.h>

mixed data;

int is_samref() { return TRUE; }

/* See element.c for the create-vs-configure rationale. */
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
