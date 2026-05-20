/*
 * LWO for XML elements.
 *
 * Lightweight wrapper carrying parsed XML element data. Instantiated by
 * xmlparse.c; structurally tagged via object_name so callers can dispatch
 * on kind without reading the data field (see queryColour /
 * queryColourValue in /lib/util/lpc).
 */

# include <std.h>

mixed data;

int is_element() { return TRUE; }

static void configure(mixed d)
{
    data = d;
}

mixed query_configuration()
{
    return data;
}

void set_data(mixed d)
{
    data = d;
}

mixed query_data()
{
    return data;
}
