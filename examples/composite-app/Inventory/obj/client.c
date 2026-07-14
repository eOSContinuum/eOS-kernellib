/*
 * Loopback HTTP/1 client for the composite example's test driver: one
 * clone per request, connecting to the platform's own binary port over
 * real TCP, so the driver exercises the full transport path (kernel
 * acceptor, mount-point clone, router, handler, daemons) rather than
 * calling the handler directly.
 *
 * This is the first in-tree consumer of the HTTP/1 client library
 * (docs/application-authoring.md Outbound connections notes that no
 * shipped example exercised it before). The binary-manager glue below
 * is replicated from /usr/HTTP/api/obj/client1.c -- the clonable form
 * under /obj/ is not inheritable -- with one deliberate difference:
 * client1.c's create calls connect() again after Http1Client's create
 * has already connected; this clone lets the library's create do the
 * connecting, once.
 *
 * Lifecycle: fetch() wires the exchange and connects; connected()
 * sends the request (plus body, held and flushed as one message);
 * receiveResponse/receiveEntity collect status and body; the driver
 * hears exactly one of http_done(code, body) or http_fail(errorcode).
 * The servers answer with Connection: close, so the peer closes and
 * the flow glue destructs this clone.
 */

# include <kernel/user.h>
# include <type.h>
# include <String.h>
# include "/usr/HTTP/api/include/HttpConnection.h"
# include "/usr/HTTP/api/include/HttpRequest.h"
# include "/usr/HTTP/api/include/HttpResponse.h"
# include "/usr/HTTP/api/include/HttpField.h"

inherit Http1Client;
inherit "/usr/System/lib/user";

# define HOST	"127.0.0.1"
# define PORT	8080		/* example.dgd binary_port */

private object driver;		/* report target */
private string method;		/* request method */
private string path;		/* request path */
private string auth;		/* Authorization header value, or nil */
private string bodyOut;		/* request body, or nil */
private int reported;		/* the driver heard exactly once */
private int responseCode;	/* collected status */

/*
 * clone-only; the exchange is wired by fetch()
 */
static void create()
{
}

/*
 * wire one exchange and connect. Called by the driver right after
 * cloning.
 */
void fetch(object drv, string m, string p, string a, string b)
{
    driver = drv;
    method = m;
    path = p;
    auth = a;
    bodyOut = b;
    ::create(this_object(), HOST, PORT,
	     OBJECT_PATH(RemoteHttpResponse), OBJECT_PATH(RemoteHttpFields));
}

private void report(string body)
{
    if (!reported) {
	reported = TRUE;
	driver->http_done(responseCode, body);
    }
}

private string drainBody(StringBuffer buf)
{
    mixed chunk;
    string acc;

    if (!buf) return "";
    acc = "";
    while ((chunk = buf->chunk()) != nil) {
	if (typeof(chunk) == T_STRING) {
	    acc += chunk;
	}
    }
    return acc;
}

/*
 * connection established: send the request, holding the serialized
 * head when a body follows so both flush as one message
 */
static void connected()
{
    HttpRequest request;
    HttpFields headers;

    request = new HttpRequest(1.1, method, nil, HOST, path);
    headers = new HttpFields();
    if (auth) {
	headers->add(new HttpField("Authorization", auth));
    }
    if (bodyOut && strlen(bodyOut) != 0) {
	headers->add(new HttpField("Content-Type", "application/json"));
	headers->add(new HttpField("Content-Length", strlen(bodyOut)));
    }
    headers->add(new HttpField("Connection", "close"));
    request->setHeaders(headers);

    sendRequest(request);
    if (bodyOut && strlen(bodyOut) != 0) {
	sendMessage(new StringBuffer(bodyOut));
    }
}

/*
 * connection failed
 */
static void connectFailed(int errorcode)
{
    if (!reported) {
	reported = TRUE;
	driver->http_fail(errorcode);
    }
}

/*
 * response status and headers parsed; opt into the body if one is
 * declared, otherwise the exchange is complete
 */
static void receiveResponse(HttpResponse response)
{
    mixed cl;
    int length;

    if (!response) {
	report("");
	return;
    }
    responseCode = response->code();
    cl = response->headerValue("Content-Length");
    length = (typeof(cl) == T_INT) ? cl : 0;
    if (length > 0) {
	expectEntity(length);
    } else {
	report("");
    }
}

/*
 * response body delivered
 */
static void receiveEntity(StringBuffer chunk)
{
    report(drainBody(chunk));
}

/*
 * Binary-manager glue replicated from /usr/HTTP/api/obj/client1.c
 * (under /obj/, not inheritable).
 */
int login(string str)
{
    if (previous_program() == LIB_CONN) {
	setMode(MODE_BLOCK);
	flow();
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
	call_out("receiveBytes", 0, str);
    }
    return TRUE;
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
