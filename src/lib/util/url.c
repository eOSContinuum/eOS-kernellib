/*
 * URL percent-encoding helpers (RFC 3986 unreserved set).
 *
 * urlEncode: percent-encode reserved characters; unreserved = A-Z a-z 0-9 - _ . ~
 * urlDecode: decode %XX escape sequences
 *
 * Mirrors the parse_string grammar pattern used by src/usr/HTTP/sys/urlencode
 * + urldecode; same RFC 3986 semantics, library-callable so Vault and other
 * subsystems do not depend on HTTP for percent-encoding.
 */

/*
 * percent-encode reserved characters (parse_string semantic callback)
 */
private string *reserved(string *parsed)
{
    int i, c, d;

    for (i = sizeof(parsed); i != 0; ) {
	c = parsed[--i][0];
	d = c >> 4;
	c &= 0x0f;
	parsed[i] = "%" + "0123456789ABCDEF"[d .. d] +
		    "0123456789ABCDEF"[c .. c] + parsed[i][1 ..];
    }
    return parsed;
}

/*
 * percent-encode a string (RFC 3986; unreserved = A-Z a-z 0-9 - _ . ~)
 */
static string urlEncode(string str)
{
    return implode(parse_string("\
unreserved = /[-_.~A-Za-z0-9]+/						\
reserved = /[^-_.~A-Za-z0-9][-_.~A-Za-z0-9]*/				\
									\
Str:									\
Str: unreserved								\
Str: unreserved Encode							\
Str: Encode								\
									\
Encode: Reserved					? reserved	\
									\
Reserved: reserved							\
Reserved: Reserved reserved", str), "");
}

# define HEX	("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09......." +	\
		 "\x0A\x0B\x0C\x0D\x0E\x0F.........................." +	\
		 "\x0a\x0b\x0c\x0d\x0e\x0f")

/*
 * decode %XX escape sequences (parse_string semantic callback)
 */
private string *escaped(string *parsed)
{
    string ch;
    int i;

    ch = ".";
    for (i = sizeof(parsed); --i >= 0; ) {
	ch[0] = (HEX[parsed[i][1] - '0'] << 4) + HEX[parsed[i][2] - '0'];
	parsed[i] = ch + parsed[i][3 ..];
    }
    return parsed;
}

/*
 * percent-decode a string
 */
static string urlDecode(string str)
{
    string *result;

    result = parse_string("\
unescaped = /[^%]+/							\
escaped = /%[0-9a-fA-F][0-9a-fA-F][^%]*/				\
junk = /./								\
									\
Str:									\
Str: unescaped								\
Str: unescaped Encoded							\
Str: Encoded								\
									\
Encoded: Escaped					? escaped	\
									\
Escaped: escaped							\
Escaped: Escaped escaped", str);

    return (result) ? implode(result, "") : nil;
}
