/*
 * LWO for XML PCDATA.
 *
 * Lightweight wrapper carrying parsed XML character data. Instantiated by
 * xmlparse.c; structurally tagged via object_name so callers can dispatch
 * on kind without reading the data field (see queryColour /
 * queryColourValue in /lib/util/lpc).
 */

# include <std.h>

mixed data;

int is_pcdata() { return TRUE; }

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
