/*
 * Round-trip codec for simple LPC values.
 *
 * encodeValue / decodeValue marshal a value to and from the LPC-literal
 * string grammar dumpValue (/lib/util/lpc.c) prints: ints, floats,
 * quoted-and-escaped strings, <name> object references, nil, ({ })
 * arrays and ([ ]) mappings. Two deliberate differences from dumpValue:
 * floats encode at full precision (float2string), and aliased or
 * cyclic structures are refused rather than printed as #N/@N
 * backreferences -- decoding a backreference would reconstruct shared
 * identity, which is a generalized serializer's job, not this codec's.
 * Light-weight objects are refused for the same reason: a reference
 * that cannot be resolved by name cannot round-trip.
 *
 * Object references encode as <logical-name> (query_object_name when
 * set, the LPC object name otherwise) and decode through find_object
 * with an Index lookup fallback, matching the OBJ() convention the
 * Schema DTD layer established.
 *
 * The encoder emits the canonical form strictly; the decoder tolerates
 * whitespace variation between tokens. Errors are loud: any value
 * outside the grammar, and any input that does not parse back, raises
 * error() at the point of refusal.
 */

# include <type.h>

private inherit "/lib/util/ascii";

# define INDEX		"/usr/Index/sys/index_daemon"


/*
 * An object's reference name: the logical name when one is set, the
 * LPC object name otherwise (the same fallback as /lib/util/lpc
 * name(); kept local so the property layer does not inherit the
 * logging/XML utility module for one helper).
 */
private string ref_name(object ob)
{
    string oname;

    oname = ob->query_object_name();
    return oname ? oname : object_name(ob);
}


private string _encodeR(mixed value, mapping seen)
{
    string str, *parts;
    int i, sz;
    mixed *indices, *values;

    switch (typeof(value)) {
    case T_NIL:
	return "nil";

    case T_INT:
	return (string) value;

    case T_FLOAT:
	return float2string(value);

    case T_STRING:
	return "\"" + replace_strings(value,
				      "\\", "\\\\",
				      "\"", "\\\"",
				      "\n", "\\n",
				      "\t", "\\t") + "\"";

    case T_OBJECT:
	if (sscanf(object_name(value), "%*s#-1") != 0) {
	    error("coercion: cannot encode a light-weight object");
	}
	return "<" + ref_name(value) + ">";

    case T_ARRAY:
	if (seen[value]) {
	    error("coercion: cannot encode an aliased or cyclic structure");
	}
	seen[value] = 1;
	sz = sizeof(value);
	parts = allocate(sz);
	for (i = 0; i < sz; i++) {
	    parts[i] = _encodeR(value[i], seen);
	}
	return "({ " + implode(parts, ", ") + " })";

    case T_MAPPING:
	if (seen[value]) {
	    error("coercion: cannot encode an aliased or cyclic structure");
	}
	seen[value] = 1;
	sz = map_sizeof(value);
	indices = map_indices(value);
	values = map_values(value);
	parts = allocate(sz);
	for (i = 0; i < sz; i++) {
	    parts[i] = _encodeR(indices[i], seen) + ":" +
		       _encodeR(values[i], seen);
	}
	return "([ " + implode(parts, ", ") + " ])";
    }
    error("coercion: cannot encode this value type");
}

/*
 * encode a simple LPC value as its literal string form
 */
static string encodeValue(mixed value)
{
    return _encodeR(value, ([ ]));
}


private int _skipws(string str, int pos)
{
    int len;

    len = strlen(str);
    while (pos < len && (str[pos] == ' ' || str[pos] == '\t' ||
			 str[pos] == '\n' || str[pos] == '\r')) {
	pos++;
    }
    return pos;
}

private mixed *_decodeR(string str, int pos);

private mixed *_decode_string(string str, int pos)
{
    string out;
    int len, start;

    len = strlen(str);
    out = "";
    start = ++pos;
    while (pos < len) {
	switch (str[pos]) {
	case '"':
	    return ({ out + str[start .. pos - 1], pos + 1 });

	case '\\':
	    out += str[start .. pos - 1];
	    if (pos + 1 >= len) {
		error("coercion: unterminated escape");
	    }
	    switch (str[pos + 1]) {
	    case '\\':
		out += "\\";
		break;
	    case '"':
		out += "\"";
		break;
	    case 'n':
		out += "\n";
		break;
	    case 't':
		out += "\t";
		break;
	    default:
		error("coercion: unknown escape at offset " +
		      (string) (pos + 1));
	    }
	    pos += 2;
	    start = pos;
	    break;

	default:
	    pos++;
	    break;
	}
    }
    error("coercion: unterminated string");
}

private mixed *_decode_object(string str, int pos)
{
    string nm;
    object ob;
    int len, start;

    len = strlen(str);
    start = ++pos;
    while (pos < len && str[pos] != '>') {
	pos++;
    }
    if (pos >= len) {
	error("coercion: unterminated object reference");
    }
    nm = str[start .. pos - 1];
    if (!strlen(nm)) {
	error("coercion: empty object reference");
    }
    ob = find_object(nm);
    if (!ob) {
	ob = INDEX->query_object(nm);
    }
    if (!ob) {
	error("coercion: no object: " + nm);
    }
    return ({ ob, pos + 1 });
}

private mixed *_decode_array(string str, int pos)
{
    mixed *out, *r;
    int len;

    len = strlen(str);
    out = ({ });
    pos = _skipws(str, pos + 2);
    if (pos + 1 < len && str[pos] == '}' && str[pos + 1] == ')') {
	return ({ out, pos + 2 });
    }
    for (;;) {
	r = _decodeR(str, pos);
	out += ({ r[0] });
	pos = _skipws(str, r[1]);
	if (pos < len && str[pos] == ',') {
	    pos++;
	    continue;
	}
	if (pos + 1 < len && str[pos] == '}' && str[pos + 1] == ')') {
	    return ({ out, pos + 2 });
	}
	error("coercion: bad array separator at offset " + (string) pos);
    }
}

private mixed *_decode_mapping(string str, int pos)
{
    mapping out;
    mixed *r;
    mixed key;
    int len;

    len = strlen(str);
    out = ([ ]);
    pos = _skipws(str, pos + 2);
    if (pos + 1 < len && str[pos] == ']' && str[pos + 1] == ')') {
	return ({ out, pos + 2 });
    }
    for (;;) {
	r = _decodeR(str, pos);
	key = r[0];
	pos = _skipws(str, r[1]);
	if (pos >= len || str[pos] != ':') {
	    error("coercion: expected : in mapping at offset " +
		  (string) pos);
	}
	r = _decodeR(str, pos + 1);
	out[key] = r[0];
	pos = _skipws(str, r[1]);
	if (pos < len && str[pos] == ',') {
	    pos++;
	    continue;
	}
	if (pos + 1 < len && str[pos] == ']' && str[pos + 1] == ')') {
	    return ({ out, pos + 2 });
	}
	error("coercion: bad mapping separator at offset " + (string) pos);
    }
}

private mixed *_decode_number(string str, int pos)
{
    string tok;
    float f;
    int len, start, isfloat, n, c;

    len = strlen(str);
    start = pos;
    if (pos < len && (str[pos] == '-' || str[pos] == '+')) {
	pos++;
    }
    while (pos < len) {
	c = str[pos];
	if (c >= '0' && c <= '9') {
	    pos++;
	} else if (c == '.' || c == 'e' || c == 'E') {
	    isfloat = 1;
	    pos++;
	    if (pos < len && (str[pos] == '-' || str[pos] == '+')) {
		pos++;
	    }
	} else {
	    break;
	}
    }
    tok = str[start .. pos - 1];
    if (!strlen(tok) || tok == "-" || tok == "+") {
	error("coercion: bad token at offset " + (string) start);
    }
    if (isfloat) {
	if (sscanf(tok, "%f", f) == 0) {
	    error("coercion: bad float literal: " + tok);
	}
	return ({ f, pos });
    }
    if (sscanf(tok, "%d", n) == 0) {
	error("coercion: bad integer literal: " + tok);
    }
    return ({ n, pos });
}

private mixed *_decodeR(string str, int pos)
{
    int len;

    len = strlen(str);
    pos = _skipws(str, pos);
    if (pos >= len) {
	error("coercion: unexpected end of input");
    }
    switch (str[pos]) {
    case 'n':
	if (pos + 3 <= len && str[pos .. pos + 2] == "nil") {
	    return ({ nil, pos + 3 });
	}
	error("coercion: bad token at offset " + (string) pos);

    case '"':
	return _decode_string(str, pos);

    case '<':
	return _decode_object(str, pos);

    case '(':
	if (pos + 1 < len && str[pos + 1] == '{') {
	    return _decode_array(str, pos);
	}
	if (pos + 1 < len && str[pos + 1] == '[') {
	    return _decode_mapping(str, pos);
	}
	error("coercion: bad token at offset " + (string) pos);
    }
    return _decode_number(str, pos);
}

/*
 * decode a literal string form back into the LPC value it encodes
 */
static mixed decodeValue(string str)
{
    mixed *r;
    int pos;

    if (!str) {
	error("coercion: no input");
    }
    r = _decodeR(str, 0);
    pos = _skipws(str, r[1]);
    if (pos != strlen(str)) {
	error("coercion: trailing text at offset " + (string) pos);
    }
    return r[0];
}
