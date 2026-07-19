/*
 * Reference HTTPS (TLS) application server for eOS-kernellib.
 *
 * Per-connection HTTPS server cloned by /usr/System/sys/https_server on
 * every incoming connection to the labeled "https" binary port. The
 * kernel-defined mount point is /usr/WWW/obj/tls_server -- copy this
 * domain to src/usr/WWW/ to deploy.
 *
 * Inherits the HTTP/1 TLS server library (Http1TlsServer, alias for
 * /usr/HTTP/api/lib/TlsServer1) for TLS session handling and request
 * parsing; and the System user-tier connection library
 * (/usr/System/lib/user) for the binary-manager contract. The
 * binary-manager glue (login, flow_*, timeout, _logout) is
 * hand-replicated from /usr/HTTP/api/obj/tls_server1 because DGD's
 * inherit_program requires the inherited path to contain "/lib/" -- the
 * clonable form under /obj/ is not inheritable. Certificate and key are
 * pulled at clone time from the https bootstrap's query_credentials
 * (the kernel clone_object passes no create arguments, so the clone
 * self-wires exactly like the plain-HTTP reference server).
 *
 * Routes (same contract as examples/http-app):
 *   GET  /health   -- returns 200 OK, body "ok\n".
 *   POST /echo     -- returns 200 OK echoing the request body.
 *   any other      -- returns 404 Not Found.
 */

# include <kernel/user.h>
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

int received;				/* received at least one request */
private HttpRequest pendingRequest;	/* awaiting body */

/*
 * On clone, pull PEM credentials from the HTTPS bootstrap and wire
 * TlsServer1's create with this_object as the server argument. Master
 * sits idle (sscanf returns 0 for the master's name, which has no
 * clone-number suffix).
 */
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

private HttpResponse makeResponse(int code, string status, string body)
{
    HttpResponse response;
    HttpFields headers;

    response = new HttpResponse(1.1, code, status);
    headers = new HttpFields();
    headers->add(new HttpField("Content-Type", "text/plain; charset=utf-8"));
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

private void dispatch(HttpRequest request, StringBuffer body)
{
    string method, path, bodyText;

    method = request->method();
    path = request->path();

    if (method == "GET" && path == "/health") {
	emit(makeResponse(200, "OK", "ok\n"), "ok\n");
	return;
    }

    if (method == "POST" && path == "/echo") {
	bodyText = drainBody(body);
	emit(makeResponse(200, "OK", bodyText), bodyText);
	return;
    }

    emit(makeResponse(404, "Not Found", "404 Not Found\n"),
	 "404 Not Found\n");
}

/*
 * Override: HTTP request parsed (decrypted stream) and headers complete.
 * Do NOT call ::receiveRequest -- the inherited Connection1 dispatch
 * relays to the server argument, which is this_object() here; chaining
 * up would infinite-loop. Body handling mirrors examples/http-app.
 */
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
    if (length > 0) {
	pendingRequest = request;
	expectEntity(length);
    } else {
	dispatch(request, nil);
    }
}

/*
 * Override: request body delivered. Retrieve the saved pendingRequest
 * and dispatch with the body.
 */
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
 * (which is under /obj/ and not inheritable). The outer stream is TLS
 * ciphertext: login begins the handshake, received records feed
 * tlsReceive, and logout closes the TLS session.
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
