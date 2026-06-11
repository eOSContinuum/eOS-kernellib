/*
 * XML entity translation.
 *
 * Translate back and forth between quoted entities in the XML manner.
 * In true XML this is a dynamic database; here a hard-coded list of the
 * XML spec's predefined entities (plus pipe / lbrace / rbrace for embedded
 * Merry-attribute escaping per the lexer convention).
 *
 * entityString builds via a bounded part-array + implode; revisit if
 * profiling shows it hot.
 */

inherit "/lib/util/lpc";

mapping asciiToEntity;
mapping entityToAscii;

static void create()
{
    asciiToEntity = ([
	'\'': "apos",
	'\"': "quot",
	'<':  "lt",
	'>':  "gt",
	'&':  "amp",
	'|':  "pipe",
	'{':  "lbrace",
	'}':  "rbrace",
    ]);

    entityToAscii = reverseMapping(asciiToEntity);
}

static string entifyString(string str, int ents...)
{
    mapping set;
    string *parts, s;
    int i, len, l;

    set = ([ ]);
    for (i = 0; i < sizeof(ents); i++) {
	if (s = asciiToEntity[ents[i]]) {
	    set[ents[i]] = "&" + s + ";";
	}
    }

    parts = ({ });
    l = 0;
    len = strlen(str);
    for (i = 0; i < len; i++) {
	if (s = set[str[i]]) {
	    if (l < i) {
		parts += ({ str[l .. i-1] });
	    }
	    parts += ({ s });
	    l = i + 1;
	}
    }
    if (l < len) {
	parts += ({ str[l ..] });
    }
    return implode(parts, "");
}

static mixed entityToAscii(string ent)
{
    return entityToAscii[ent];
}

static string asciiToEntity(int ascii)
{
    return asciiToEntity[ascii];
}
