/*
 * Loopback SSE client for the composite example's test driver: the
 * streaming sibling of obj/client.c, and the receive-side first
 * consumer of the chunked-transfer surface (expectChunk /
 * receiveChunk).
 *
 * Where obj/client.c runs one exchange to completion, this clone opens
 * a GET, reports the response head via stream_open(code), opts into
 * chunked receipt, and then reports every complete server-sent-event
 * frame via stream_event(event, data) for as long as the stream stays
 * open. The driver owns teardown: it destructs the clone (the peer
 * sees the close) when a phase is done with the stream.
 *
 * The request deliberately omits Connection: close -- the point is a
 * held-open connection. Framing follows the SSE wire form the WWW
 * servers emit: "event: <name>\ndata: <json>\n\n" per event, each
 * arriving as one or more chunks.
 */

# include <kernel/user.h>
# include <type.h>
# include <String.h>
# include "/usr/HTTP/api/include/HttpConnection.h"
# include "/usr/HTTP/api/include/HttpRequest.h"
# include "/usr/HTTP/api/include/HttpResponse.h"
# include "/usr/HTTP/api/include/HttpField.h"

inherit client Http1Client;
inherit buffered "/usr/HTTP/api/lib/BufferedConnection1";
inherit user "/usr/System/lib/user";

# define HOST	"127.0.0.1"
# define PORT	8080		/* example.dgd binary_port */

private object driver;		/* report target */
private string path;		/* request path */
private string acc;		/* partial-frame accumulator */

static void create()
{
}

/*
 * wire the stream and connect
 */
void open(object drv, string p)
{
    driver = drv;
    path = p;
    acc = "";
    client::create(this_object(), HOST, PORT,
		   OBJECT_PATH(RemoteHttpResponse),
		   OBJECT_PATH(RemoteHttpFields));
    buffered::create(MODE_LINE);
}

static void connected()
{
    HttpRequest request;
    HttpFields headers;

    request = new HttpRequest(1.1, "GET", nil, nil, path);
    headers = new HttpFields();
    headers->add(new HttpField("Host", HOST));
    request->setHeaders(headers);
    this_object()->sendRequest(request);
}

static void connectFailed(int errorcode)
{
    driver->stream_fail(errorcode);
}

/*
 * response head: report the status, then opt into the chunk stream
 */
static void receiveResponse(HttpResponse response)
{
    if (!response) {
	driver->stream_fail(-1);
	return;
    }
    driver->stream_open(response->code());
    if (response->code() == 200) {
	expectChunk();
    }
}

private string drainBody(StringBuffer buf)
{
    mixed chunk;
    string str;

    if (!buf) return "";
    str = "";
    while ((chunk = buf->chunk()) != nil) {
	if (typeof(chunk) == T_STRING) {
	    str += chunk;
	}
    }
    return str;
}

/*
 * one transfer chunk: accumulate, then report every complete SSE
 * frame ("event: <name>\ndata: <data>\n\n")
 */
static void receiveChunk(StringBuffer chunk, varargs HttpFields trailers)
{
    string frame, rest, event, data;

    acc += drainBody(chunk);
    while (sscanf(acc, "%s\n\n%s", frame, rest) != 0) {
	acc = rest;
	if (sscanf(frame, "event: %s\ndata: %s", event, data) != 0) {
	    driver->stream_event(event, data);
	}
    }
    expectChunk();
}

/*
 * Binary-manager glue: same raw-mode shape as obj/client.c
 */
int login(string str)
{
    if (previous_program() == LIB_CONN) {
	flow();
	flow_mode(MODE_RAW);
	call_limited("connected");
    }
    return MODE_NOCHANGE;
}

void connect_failed(int errorcode)
{
    if (previous_program() == LIB_CONN) {
	call_limited("connectFailed", errorcode);
	destruct_object(this_object());
    }
}

int flow_receive_message(string str, int mode)
{
    if (previous_program() == LIB_CONN) {
	call_out("receive_raw", 0, str);
    }
    return TRUE;
}

static void receive_raw(string str)
{
    buffered::receiveBytes(new StringBuffer(str));
}

static void _logout(int quit)
{
    call_limited("close", quit);
    destruct_object(this_object());
}

void flow_logout(int quit)
{
    if (previous_program() == LIB_CONN) {
	call_out("_logout", 0, quit);
    }
}

void flow_message_done()
{
    if (previous_program() == LIB_CONN) {
	call_limited("messageDone");
    }
}
