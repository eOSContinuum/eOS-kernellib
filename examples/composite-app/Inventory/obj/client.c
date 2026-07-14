/*
 * Loopback HTTP/1 client for the composite example's test driver: one
 * clone per request, connecting to the platform's own binary port over
 * real TCP, so the driver exercises the full transport path (kernel
 * acceptor, mount-point clone, router, handler, daemons) rather than
 * calling the handler directly.
 *
 * This is the first in-tree consumer of the HTTP/1 client library
 * (docs/application-authoring.md Outbound connections notes that no
 * shipped example exercised it before), and its shape follows the TLS
 * pair (Http1TlsClient / obj/tls_client1.c), not obj/client1.c. The
 * plain clonable's driver-line-mode design has two failure modes this
 * example hit live:
 *
 *   - MODE_BLOCK at login is never lifted (only MODE_UNBLOCK lifts
 *     driver-level blocking), so the response sits unread in the
 *     socket; and
 *   - with the driver line-framing the input, a response head and
 *     body arriving in one TCP segment queue the body as a LINE event
 *     before expectEntity's RAW switch lands -- the body line then
 *     parses as a new status line ("Bad response"), and the stripped
 *     line terminator makes the Content-Length count unsatisfiable.
 *
 * The cure is the same one the TLS variants use: keep the DRIVER in
 * raw mode for the whole exchange and let BufferedConnection1 do the
 * line/entity framing internally, synchronously with Connection1's
 * state machine (its setMode override keeps mode changes local).
 *
 * Lifecycle: fetch() wires the exchange and connects; connected()
 * sends the request (body queued behind the head's call_out);
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

inherit client Http1Client;
inherit buffered "/usr/HTTP/api/lib/BufferedConnection1";
inherit user "/usr/System/lib/user";

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
    client::create(this_object(), HOST, PORT,
		   OBJECT_PATH(RemoteHttpResponse),
		   OBJECT_PATH(RemoteHttpFields));
    buffered::create(MODE_LINE);
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
 * connection established: send the request; a body follows the head
 * in queue order (0-delay call_outs run in the order queued, and the
 * flow wrapper defers the head send by one)
 */
static void connected()
{
    HttpRequest request;
    HttpFields headers;

    /* origin-form request line: scheme and host nil (transport()
     * would otherwise serialize them into the request-target); the
     * host travels in the Host header */
    request = new HttpRequest(1.1, method, nil, nil, path);
    headers = new HttpFields();
    headers->add(new HttpField("Host", HOST));
    if (auth) {
	headers->add(new HttpField("Authorization", auth));
    }
    if (bodyOut && strlen(bodyOut) != 0) {
	headers->add(new HttpField("Content-Type", "application/json"));
	headers->add(new HttpField("Content-Length", strlen(bodyOut)));
    }
    /* comma-list fields are array-valued (Field's listContains
     * assumes it; Connection1 consults this header on send) */
    headers->add(new HttpField("Connection", ({ "close" })));
    request->setHeaders(headers);

    /* through call_other so the flow wrapper sees previous_object()
     * == the relay (this object) */
    this_object()->sendRequest(request);
    if (bodyOut && strlen(bodyOut) != 0) {
	call_out("send_body", 0);
    }
}

static void send_body()
{
    sendMessage(new StringBuffer(bodyOut));
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
 * Binary-manager glue modeled on /usr/HTTP/api/obj/tls_client1.c: the
 * driver-level connection stays in raw mode for the whole exchange
 * (BufferedConnection1 does the framing), input feeds the buffered
 * layer, and no driver-level MODE_BLOCK is ever set.
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
