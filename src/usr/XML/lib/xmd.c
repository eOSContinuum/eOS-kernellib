/*
 * XMD (xtended markup data) structure helpers.
 *
 * XMD is the internal binary form of XML; the name XML belongs to the
 * ASCII serialization. These helpers construct, query, and reshape XMD
 * trees built from the LWO data wrappers in /usr/XML/data/.
 *
 * Lifted from skoot/usr/XML/lib/xmd.c. LV-4.5a refactors:
 * (a) /lib/array + /lib/string inherits replaced by /lib/util/ascii +
 *     /lib/util/lpc helper inherits.
 * (b) Function names refactored to camelCase per LV-2.5 / LV-2.5b. Calls
 *     into the lifted XML transport (queryColourValue / queryColour /
 *     sysLog / dumpValue / member) use the lifted names.
 * (c) lower_case and the strip helpers retain snake_case in ascii.c
 *     (lower_case is pre-existing in cloud-server's kernel ascii lib;
 *     strip / strip_left / strip_right added at LV-4.5a alongside).
 */

# include <type.h>
# include <XML.h>

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";

static mixed xmdElts(string name, mixed *attr, mixed subelts...)
{
    return NewXMLElement(({ lower_case(name), attr, subelts }));
}

static mixed xmdText(string name, mixed *attr, mixed value)
{
    return NewXMLElement(({ lower_case(name), attr, value }));
}

static mixed xmdElement(mixed elt)    { return queryColourValue(elt)[0]; }
static mixed xmdAttributes(mixed elt) { return queryColourValue(elt)[1]; }
static mixed xmdContent(mixed elt)    { return queryColourValue(elt)[2]; }

static mixed xmdRef(string ref, varargs mixed *attr)
{
    return NewXMLSAMRef(({ ref, attr ? attr : ({ }) }));
}

static string xmdRefRef(mixed ref)        { return queryColourValue(ref)[0]; }
static mixed *xmdRefAttributes(mixed ref) { return queryColourValue(ref)[1]; }

static mixed *xmdWipeTags(mixed elts, string tags...)
{
    int i;

    elts = queryColourValue(elts)[..];	/* copy */

    for (i = 0; i < sizeof(elts); i++) {
	if (typeof(elts[i]) == T_ARRAY && member(xmdElement(elts[i]), tags)) {
	    elts[i] = nil;
	}
    }
    return elts - ({ nil });
}

# define WS(n)	((n) == ' ' || (n) == '\n')

static mixed xmdStripPcdata(mixed xmd)
{
    if (typeof(queryColourValue(xmd)) == T_STRING) {
	return NewXMLPCData(strip(queryColourValue(xmd)));
    }
    if (typeof(queryColourValue(xmd)) == T_NIL) {
	return NewXMLPCData(nil);
    }
    if (typeof(queryColourValue(xmd)) != T_ARRAY) {
	error("not xmd: " + dumpValue(xmd));
    }
    if (queryColour(xmd) == COL_SAMREF || queryColour(xmd) == COL_ELEMENT) {
	/* done */
	return xmd;
    }
    /* Must be PCDATA then. */
    xmd = queryColourValue(xmd);
    if (sizeof(xmd) > 0) {
	mixed str;

	xmd = xmd[..];	/* copy */

	if (typeof(xmd[0]) == T_STRING) {
	    str = strip_left(xmd[0]);
	    if (!strlen(str)) {
		xmd = xmd[1 ..];
	    } else {
		xmd[0] = str;
	    }
	}
	str = xmd[sizeof(xmd)-1];
	if (typeof(str) == T_STRING) {
	    str = strip_right(str);
	    if (!strlen(str)) {
		xmd = xmd[.. sizeof(xmd)-2];
	    } else {
		xmd[sizeof(xmd)-1] = str;
	    }
	}
    }
    return NewXMLPCData(xmd);
}

static mixed xmdForceToData(mixed xmd)
{
    int i, j;

    if (!xmd) {
	/* nil is a valid element sequence */
	return NewXMLPCData(nil);
    }
    if (typeof(xmd) == T_STRING) {
	if (xmd == "" || xmd == " " || xmd == "\n") {
	    return NewXMLPCData(nil);
	}
	error("expecting data, got " + dumpValue(xmd));
    }

    /* it had better be an array now */
    if (queryColour(xmd) == COL_ELEMENT) {
	/* an element is definitely data: return PCDATA */
	return NewXMLPCData(({ xmd }));
    }
    /* otherwise assume it's PCDATA already: check the guts */
    xmd = queryColourValue(xmd)[..];	/* copy */

    if (typeof(xmd) == T_STRING) {
	sysLog("Got raw string in xmd: " + xmd);
	return NewXMLPCData(nil);
    }

    for (i = 0; i < sizeof(xmd); i++) {
	mixed xmd_i;

	xmd_i = queryColourValue(xmd[i]);
	if (typeof(xmd_i) == T_STRING) {
	    for (j = 0; j < strlen(xmd_i); j++) {
		switch (xmd_i[j]) {
		case ' ': case '\n':
		    /* OK, whitespace */
		    break;
		default:
		    error("expecting data, got " + dumpValue(xmd_i));
		}
	    }
	    xmd[i] = nil;
	}
    }
    return NewXMLPCData(xmd - ({ nil }));
}


static mapping attributesToMapping(mixed *attr)
{
    mapping map;
    int i;

    map = ([ ]);
    for (i = 0; i < sizeof(attr); i += 2) {
	map[attr[i]] = attr[i+1];
    }
    return map;
}

static mixed xmdOptimize(mixed xmd)
{
    mixed xmd_value;

    while (typeof(xmd_value = queryColourValue(xmd)) == T_ARRAY &&
	   sizeof(xmd_value) == 1) {
	xmd = xmd_value[0];
    }
    return xmd;
}
