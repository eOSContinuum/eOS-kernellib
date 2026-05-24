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

/* LV-4.5d: System/lib/auto._F_init dispatches new_object(path, args...) to
 * create(args...), not configure(args...). LWO data wrappers carry their
 * payload as a single mixed via create(). */
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
