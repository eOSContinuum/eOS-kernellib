/*
 * Routing HTTP/1 application server for the composite example.
 *
 * Per-connection HTTP server cloned by /usr/System/sys/http_server on
 * every incoming connection to the binary port; the kernel-defined
 * mount point is /usr/WWW/obj/server. Where examples/http-app answers
 * its two routes inline, this server carries no application logic at
 * all: it resolves the request path through the route registry
 * (sys/router.c) and relays to the owning domain's handler. This is
 * the runnable form of docs/http-applications.md "Multiple
 * applications on one port".
 *
 * The handler contract is deliberately narrow:
 *
 *   mixed *handle(string method, string path, string body,
 *                 string authorization)
 *     -> ({ int code, string phrase, string contentType, string body })
 *
 * The server stays the only object that touches HttpRequest/HttpResponse
 * wire objects; handlers see strings and return strings. A handler
 * error becomes a 500 without dropping the connection contract.
 *
 * Inheritance, clone setup, and the binary-manager glue are replicated
 * from examples/http-app/obj/server.c -- see that file and
 * docs/http-applications.md for the platform contracts (library-form
 * inheritance, the receiveRequest override rule, expectEntity).
 */

# include <kernel/user.h>
# include <type.h>
# include <String.h>
# include "/usr/HTTP/api/include/HttpConnection.h"
# include "/usr/HTTP/api/include/HttpRequest.h"
# include "/usr/HTTP/api/include/HttpResponse.h"
# include "/usr/HTTP/api/include/HttpField.h"

inherit Http1Server;
inherit "/usr/System/lib/user";

# define ROUTER		"/usr/WWW/sys/router"

int received;				/* received at least one request */
private HttpRequest pendingRequest;	/* awaiting body */

static void create()
{
    if (sscanf(object_name(this_object()), "%*s#") != 0) {
	::create(this_object(), OBJECT_PATH(RemoteHttpRequest),
		 OBJECT_PATH(RemoteHttpFields));
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
}

private void emitPlain(int code, string status, string body)
{
    emit(makeResponse(code, status, "text/plain; charset=utf-8", body),
	 body);
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
 * Resolve the path through the route registry and relay to the owning
 * domain's handler. No registered prefix is a 404; a registered but
 * unloaded handler is a 503; a handler error or a malformed handler
 * return is a 500.
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

    auth = request->headerValue("Authorization");
    if (catch(result = handler->handle(request->method(), path,
				       drainBody(body),
				       (typeof(auth) == T_STRING) ?
					auth : nil)) != nil ||
	typeof(result) != T_ARRAY || sizeof(result) != 4) {
	emitPlain(500, "Internal Server Error", "500 handler error\n");
	return;
    }
    emit(makeResponse(result[0], result[1], result[2], result[3]),
	 result[3]);
}

/*
 * Override: HTTP request parsed and headers complete. Do NOT call
 * ::receiveRequest (the relay is this object; chaining up would
 * recurse). Body-bearing methods opt into receipt via expectEntity;
 * the body arrives in receiveEntity below.
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
 * Override: request body delivered
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
 * Binary-manager glue replicated from /usr/HTTP/api/obj/server1 (under
 * /obj/, not inheritable) -- see examples/http-app/obj/server.c.
 */
int login(string str)
{
    if (previous_program() == LIB_CONN) {
	::connection(previous_object());
	flow();
	return call_limited("receiveFirstLine", str);
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
	call_out("messageDone", 0);
    }
}

int timeout()
{
    if (previous_program() == LIB_CONN) {
	return !received;
    }
}
