# include <limits.h>
# include <type.h>

/*
 * CBOR decoder (RFC 8949) for the deterministic subset that WebAuthn
 * and CTAP2 payloads use: integers, byte strings, text strings,
 * arrays, maps, and the simple values false/true/null. Indefinite
 * lengths, tags, floats, and undefined are rejected -- CTAP2 canonical
 * encoding forbids the first two, and the identity stack has no use
 * for the rest. Decoding is strict: truncation, malformed initial
 * bytes, duplicate map keys, and integers that do not fit an LPC int
 * all raise errors rather than returning partial values.
 *
 * Values map to LPC as: unsigned/negative integer -> int, byte and
 * text string -> string, array -> array, map -> mapping (keys must be
 * integers or strings), false/true -> 0/1, null -> nil. A nil-valued
 * map entry cannot be represented (assigning nil deletes the entry),
 * so null map values are rejected.
 */

# define CBOR_MAX_DEPTH		32

private mixed *item(string data, int offset, int depth);

/*
 * read a big-endian unsigned integer of nbytes at offset; error if it
 * does not fit a (signed) LPC int
 */
private int uintAt(string data, int offset, int nbytes)
{
    int value, i;

    if (offset + nbytes > strlen(data)) {
	error("cbor: truncated");
    }
# if INT_MIN == 0x80000000
    for (i = 0; i < nbytes - 4; i++) {
	if (data[offset + i] != 0) {
	    error("cbor: integer out of range");
	}
    }
    if (nbytes >= 4 && (data[offset + nbytes - 4] & 0x80)) {
	error("cbor: integer out of range");
    }
# else
    if (nbytes == 8 && (data[offset] & 0x80)) {
	error("cbor: integer out of range");
    }
# endif
    value = 0;
    for (i = 0; i < nbytes; i++) {
	value = (value << 8) | data[offset + i];
    }
    return value;
}

/*
 * decode one data item at offset; return ({ value, offset past item })
 */
private mixed *item(string data, int offset, int depth)
{
    int initial, major, info, arg, i;
    mixed *sub, *arr, *keys;
    mixed key, value;
    mapping map;

    if (depth > CBOR_MAX_DEPTH) {
	error("cbor: nesting too deep");
    }
    if (offset >= strlen(data)) {
	error("cbor: truncated");
    }
    initial = data[offset];
    major = initial >> 5;
    info = initial & 0x1f;
    offset++;

    switch (info) {
    case 0 .. 23:
	arg = info;
	break;

    case 24:
	arg = uintAt(data, offset, 1);
	offset += 1;
	break;

    case 25:
	arg = uintAt(data, offset, 2);
	offset += 2;
	break;

    case 26:
	arg = uintAt(data, offset, 4);
	offset += 4;
	break;

    case 27:
	arg = uintAt(data, offset, 8);
	offset += 8;
	break;

    case 28 .. 30:
	error("cbor: malformed initial byte");

    case 31:
	error("cbor: indefinite length not supported");
    }

    switch (major) {
    case 0:	/* unsigned integer */
	return ({ arg, offset });

    case 1:	/* negative integer */
	return ({ -1 - arg, offset });

    case 2:	/* byte string */
    case 3:	/* text string */
	if (offset + arg > strlen(data)) {
	    error("cbor: truncated");
	}
	if (arg == 0) {
	    return ({ "", offset });
	}
	return ({ data[offset .. offset + arg - 1], offset + arg });

    case 4:	/* array */
	/* every element takes at least one byte; reject a declared
	 * count the remaining data cannot hold before allocating */
	if (arg > strlen(data) - offset) {
	    error("cbor: truncated");
	}
	arr = allocate(arg);
	for (i = 0; i < arg; i++) {
	    sub = item(data, offset, depth + 1);
	    arr[i] = sub[0];
	    offset = sub[1];
	}
	return ({ arr, offset });

    case 5:	/* map */
	/* every entry takes at least two bytes (key + value) */
	if (arg > (strlen(data) - offset) >> 1) {
	    error("cbor: truncated");
	}
	map = ([ ]);
	keys = allocate(arg);
	for (i = 0; i < arg; i++) {
	    sub = item(data, offset, depth + 1);
	    key = sub[0];
	    offset = sub[1];
	    switch (typeof(key)) {
	    case T_INT:
	    case T_STRING:
		break;

	    default:
		error("cbor: map key must be an integer or a string");
	    }
	    if (sizeof(keys[.. i - 1] & ({ key })) != 0) {
		error("cbor: duplicate map key");
	    }
	    keys[i] = key;
	    sub = item(data, offset, depth + 1);
	    value = sub[0];
	    offset = sub[1];
	    if (value == nil) {
		error("cbor: null map value not representable");
	    }
	    map[key] = value;
	}
	return ({ map, offset });

    case 6:	/* tag */
	error("cbor: tags not supported");

    case 7:	/* simple values and floats */
	switch (info) {
	case 20:	/* false */
	    return ({ 0, offset });

	case 21:	/* true */
	    return ({ 1, offset });

	case 22:	/* null */
	    return ({ nil, offset });

	case 23:	/* undefined */
	    error("cbor: undefined not supported");

	case 24:	/* simple value in following byte */
	    error("cbor: simple values not supported");

	default:	/* 25, 26, 27: half, single, double float */
	    error("cbor: floats not supported");
	}
    }
}

/*
 * decode the data item starting at offset; return ({ value, next })
 * where next is the offset of the first byte past the item
 */
static mixed *decodePrefix(string data, int offset)
{
    if (offset < 0 || offset > strlen(data)) {
	error("cbor: offset out of range");
    }
    return item(data, offset, 0);
}

/*
 * decode a string holding exactly one data item
 */
static mixed decode(string data)
{
    mixed *result;

    result = item(data, 0, 0);
    if (result[1] != strlen(data)) {
	error("cbor: trailing bytes after item");
    }
    return result[0];
}
