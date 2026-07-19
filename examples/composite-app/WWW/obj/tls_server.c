/*
 * Routing HTTPS (TLS) application server for the composite example.
 *
 * Per-connection HTTPS server cloned by /usr/System/sys/https_server on
 * every incoming connection to the labeled "https" binary port; the
 * kernel-defined mount point is /usr/WWW/obj/tls_server. Dispatch is
 * identical to obj/server.c -- the route registry and the handler
 * contract are transport-indifferent, which is the point: the same
 * Inventory handlers serve both the plain and the TLS mount without
 * knowing which carried the request.
 *
 * This mount is not exercised by the headless run-example.sh profile
 * (it needs certificates on the https port -- see docs/operations.md
 * Network boundary and transport security); it exists so a browser-real
 * session against the same deployed example is a deploy away. TLS
 * setup, clone self-wiring, and the binary-manager glue are replicated
 * from examples/https-app/obj/tls_server.c.
 */

# include <kernel/user.h>
# include <status.h>
# include <type.h>
# include <String.h>
# include "/usr/TLS/api/include/tls.h"
# include "/usr/HTTP/api/include/HttpConnection.h"
# include "/usr/HTTP/api/include/HttpRequest.h"
# include "/usr/HTTP/api/include/HttpResponse.h"
# include "/usr/HTTP/api/include/HttpField.h"

inherit Http1TlsServer;
inherit "/usr/System/lib/user";

# define HTTPS_SERVER	"/usr/System/sys/https_server"
# define ROUTER		"/usr/WWW/sys/router"

int received;				/* received at least one request */
private HttpRequest pendingRequest;	/* awaiting body */
private int streaming;			/* holding an event stream open */

static void create()
{
    if (sscanf(object_name(this_object()), "%*s#") != 0) {
	string cert, key;

	({ cert, key }) = HTTPS_SERVER->query_credentials();
	::create(this_object(), cert, key,
		 OBJECT_PATH(RemoteHttpRequest),
		 OBJECT_PATH(RemoteHttpFields),
		 OBJECT_PATH(TlsServerSession));
    }
}

private HttpResponse makeResponse(int code, string status, string ctype,
				  string body)
{
    HttpResponse response;
    HttpFields headers;

    response = new HttpResponse(1.1, code, status);
    headers = new HttpFields();
    headers->add(new HttpField("Content-Type", ctype));
    headers->add(new HttpField("Content-Length", strlen(body)));
    headers->add(new HttpField("Connection", "close"));
    response->setHeaders(headers);
    return response;
}

private void emit(HttpResponse response, string body)
{
    StringBuffer message;

    message = new StringBuffer(response->transport());
    if (body && strlen(body) != 0) {
	message->append(body);
    }
    sendMessage(message);
    /* the response is complete: release the request so the flow layer
     * closes or re-arms the connection and the user slot recycles */
    this_object()->doneRequest();
}

private void emitPlain(int code, string status, string body)
{
    emit(makeResponse(code, status, "text/plain; charset=utf-8", body),
	 body);
}

/*
 * streaming support, identical to obj/server.c: sentinel handler
 * return switches to chunked streaming; a broker pushes frames via
 * push_event / end_stream (call_other-to-self for the relay guard;
 * the empty params array opts into chunk framing)
 */
private void startStream(int code, string status)
{
    HttpResponse response;
    HttpFields headers;

    response = new HttpResponse(1.1, code, status);
    headers = new HttpFields();
    headers->add(new HttpField("Content-Type", "text/event-stream"));
    headers->add(new HttpField("Cache-Control", "no-cache"));
    headers->add(new HttpField("Transfer-Encoding", ({ "chunked" })));
    response->setHeaders(headers);
    streaming = TRUE;
    sendMessage(new StringBuffer(response->transport()));
}

void push_event(string event, string data)
{
    if (sscanf(previous_program(), "/usr/%*s/sys/%*s") == 0) {
	error("Access denied");
    }
    if (streaming) {
	this_object()->sendChunk(new StringBuffer("event: " + event +
						  "\ndata: " + data + "\n\n"),
				 ({ }));
    }
}

void end_stream()
{
    if (sscanf(previous_program(), "/usr/%*s/sys/%*s") == 0) {
	error("Access denied");
    }
    if (streaming) {
	streaming = FALSE;
	this_object()->endChunk();
	call_out("_logout", 0, TRUE);
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
 * Same dispatch as obj/server.c: registry lookup, relay, narrow
 * handler contract.
 */
private void dispatch(HttpRequest request, StringBuffer body)
{
    string path, hpath;
    object handler;
    mixed auth, *result;

    path = request->path();
    hpath = ROUTER->query_handler(path);
    if (!hpath) {
	emitPlain(404, "Not Found", "404 Not Found\n");
	return;
    }
    handler = find_object(hpath);
    if (!handler) {
	emitPlain(503, "Service Unavailable", "503 handler not loaded\n");
	return;
    }

    /* the HTTP layer parses Authorization into a value object
     * (/usr/HTTP/api/lib/Authentication.c); hand the handler the
     * wire form it re-serializes to */
    auth = request->headerValue("Authorization");
    if (typeof(auth) == T_OBJECT) {
	auth = auth->transport();
    }
    if (catch(result = handler->handle(request->method(), path,
				       drainBody(body),
				       (typeof(auth) == T_STRING) ?
					auth : nil)) != nil ||
	typeof(result) != T_ARRAY || sizeof(result) != 4) {
	emitPlain(500, "Internal Server Error", "500 handler error\n");
	return;
    }
    if (result[2] == "text/event-stream" && result[3] == nil) {
	startStream(result[0], result[1]);
	return;
    }
    emit(makeResponse(result[0], result[1], result[2], result[3]),
	 result[3]);
}

static void receiveRequest(int code, HttpRequest request)
{
    string method;
    mixed cl;
    int length;

    received = TRUE;

    if (code != 0 || !request) {
	return;
    }

    method = request->method();
    if (method == "GET" || method == "HEAD" || method == "DELETE") {
	dispatch(request, nil);
	return;
    }

    cl = request->headerValue("Content-Length");
    length = (typeof(cl) == T_INT) ? cl : 0;
    if (length > status()[ST_STRSIZE]) {
	/* larger than one LPC string can hold: refuse before reading
	 * rather than erroring mid-drain; the response closes the
	 * connection, discarding the unread body */
	emitPlain(413, "Payload Too Large", "413 Payload Too Large\n");
	return;
    }
    if (length > 0) {
	pendingRequest = request;
	expectEntity(length);
    } else {
	dispatch(request, nil);
    }
}

static void receiveEntity(StringBuffer chunk)
{
    HttpRequest request;

    if (pendingRequest) {
	request = pendingRequest;
	pendingRequest = nil;
	dispatch(request, chunk);
    }
}

/*
 * Binary-manager glue replicated from /usr/HTTP/api/obj/tls_server1
 * (under /obj/, not inheritable) -- see examples/https-app.
 */
int login(string str)
{
    if (previous_program() == LIB_CONN) {
	::connection(previous_object());
	flow();
	call_limited("tlsAccept", str, 0);
    }
    return MODE_NOCHANGE;
}

int flow_receive_message(string str, int mode)
{
    if (previous_program() == LIB_CONN) {
	call_out("tlsReceive", 0, str);
    }
    return TRUE;
}

static void _logout(int quit)
{
    tlsClose(quit);
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
	call_out("messageDone", 0);
    }
}

int timeout()
{
    if (previous_program() == LIB_CONN) {
	return !received;
    }
}
