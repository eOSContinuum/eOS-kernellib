/*
 * XML generation from internal XMD data format.
 *
 * Generates ASCII XML from XMD trees built by xmd.c. Output is appended
 * to a caller-supplied object via `append(string)` (StringBuffer-shape API);
 * the lifted xml_daemon.c clones a /lib/StringBuffer per gen_xml call.
 *
 * generate_pcdata dispatches to element / pcdata / samref directly;
 * the COL_SAMREF case emits a literal $(ref attrs) form for any reader
 * that wants it, with no game-content interpretation.
 */

# include <type.h>
# include <XML.h>

# define SID		"/usr/Schema/sys/schema_daemon"

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";

inherit "/usr/XML/lib/entities";
private inherit "/usr/XML/lib/xmd";
private inherit "/usr/Schema/lib/dtd";

# define RIGHT_MARGIN	70

static string generate_pcdata(mixed pcdata);

# define EscapePCData(str) \
   replace_strings(str,		\
	  "\\", "\\\\",         \
	  "&", "\\&",		\
	  "$", "\\$",		\
	  "<", "\\<",		\
	  ">", "\\>",		\
	  "{", "\\{",		\
	  "}", "\\}",		\
	  "|", "\\|")

string fiddle_ref(string ref)
{
    /* this check should be wider, but in practice this is what happens */
    if (sscanf(ref, "%*s ")) {
	return "\"" + ref + "\"";
    }
    return ref;
}

private string xml_attr(string tag, mixed *attr)
{
    string str, type, astr;
    int i;

    str = "";
    if (!attr) {
	return str;
    }
    for (i = 0; i < sizeof(attr); i += 2) {
	if (attr[i+1] != nil) {
	    if (SID->query_node(tag)) {
		type = SID->get_node(tag)->query_attribute_type(attr[i]);
	    } else {
		type = nil;
	    }
	    if (type == XML_SAMREF || queryColour(attr[i+1]) == COL_SAMREF || queryColour(attr[i+1]) == COL_PCDATA) {
		astr = generate_pcdata(attr[i+1]);
	    } else {
		astr = typed_to_ascii(attr[i+1], type);
		astr = replace_strings(astr, "\"", "\\\"");
		if (type != XML_PCDATA && queryColour(attr[i+1]) != COL_PCDATA) {
		    astr = replace_strings(astr, "$", "\\$");
		}
	    }
	    str += " " + fiddle_ref(attr[i]) + "=\"" + astr + "\"";
	}
    }
    return str;
}

private string xml_head(string tag, mixed *attr, mixed body)
{
    string head;

    head = "<" + tag + xml_attr(tag, attr);

    if (body == nil) {
	return head + "/>";
    }
    return head + ">";
}

static string generate_pcdata(mixed pcdata)
{
    if (!pcdata || !queryColourValue(pcdata)) {
	return "";
    }
    switch (queryColour(pcdata)) {
    case COL_ELEMENT: {
	object node;
	string tag, type, body;

	if (node = SID->query_node(xmdElement(pcdata))) {
	    tag = node->query_name();
	} else {
	    tag = xmdElement(pcdata);
	}

	if (xmdContent(pcdata) == nil ||
	    (typeof(xmdContent(pcdata)) == T_ARRAY  && sizeof(xmdContent(pcdata)) == 0) ||
	    (typeof(xmdContent(pcdata)) == T_STRING && strlen(xmdContent(pcdata)) == 0)) {
	    return xml_head(tag, xmdAttributes(pcdata), nil);
	}
	type = node ? node->query_type() : nil;
	body = typed_to_ascii(xmdContent(pcdata), type);

	/* try to figure out when escaping has already been done */
	if ((type || typeof(xmdContent(pcdata)) == T_STRING) &&
	    type != XML_PCDATA && queryColour(xmdContent(pcdata)) != COL_PCDATA) {
	    body = EscapePCData(body);
	}
	return xml_head(tag, xmdAttributes(pcdata), xmdContent(pcdata)) +
	    body + "</" + tag + ">";
    }
    case COL_SAMREF: {
	return "$(" + fiddle_ref(xmdRefRef(pcdata)) +
	    xml_attr(xmdRefRef(pcdata), xmdRefAttributes(pcdata)) + ")";
    }
    case COL_PCDATA: {
	string res;
	int i;

	pcdata = queryColourValue(pcdata);
	if (typeof(pcdata) == T_STRING) {
	    return EscapePCData(pcdata);
	}
	if (typeof(pcdata) == T_ARRAY) {
	    res = "";
	    for (i = 0; i < sizeof(pcdata); i++) {
		res += generate_pcdata(pcdata[i]);
	    }
	}
	return res;
    }
    }
    return EscapePCData(untyped_to_ascii(pcdata));
}

static void generate_xml(mixed data, object res, varargs string indent)
{
    object node;
    string tag, head, body, tail, type;
    int i;

    if (!indent) {
	indent = "";
    }

    if (data == nil) {
	return;
    }
    data = queryColourValue(data);
    if (typeof(data) == T_STRING) {
	res->append(EscapePCData(data));
	return;
    }
    if (typeof(data) != T_ARRAY) {
	error("weird value"); /* this will change in time TODO */
    }
    if (node = SID->query_node(data[0])) {
	tag = node->query_name();
    } else {
	tag = data[0];
    }

    type = node ? node->query_type() : nil;

    if (data[2] == nil ||
	(type != "lpc_mixed"
	    && typeof(data[2]) == T_ARRAY
	    && sizeof(data[2]) == 0
	) ||
	(typeof(data[2]) == T_STRING && strlen(data[2]) == 0)
    ) {
	res->append(indent + xml_head(tag, data[1], nil) + "\n");
	return;
    }

    head = xml_head(tag, data[1], data[2]);
    tail = "</" + tag + ">\n";

    if (!node || node->is_parent()) {
	res->append(indent + head + "\n");
	for (i = 0; i < sizeof(data[2]); i++) {
	    if (typeof(data[2][i]) != T_STRING || data[2][i] != " ") {
		generate_xml(data[2][i], res, indent + "  ");
	    }
	}
	res->append(indent + tail);
	return;
    }

    body = typed_to_ascii(data[2], type);
    if (type != XML_PCDATA) {
	body = EscapePCData(body);
    }
    if (strlen(head + body + tail) + strlen(indent) < RIGHT_MARGIN) {
	res->append(indent + head + body + tail);
	return;
    }
    body = strip(body);
    res->append(indent + head + "\n" +
		implode(explode(indent + "   " + body, "\n"), "\n") + "\n" +
		indent + tail);
}
