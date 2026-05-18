/*
 * HTTP/1 server for the atomic-rollback demonstration.
 *
 * Per-connection HTTP server cloned by /usr/System/sys/http_server on every
 * incoming connection to the binary port. The kernel-defined mount point is
 * /usr/WWW/obj/server -- copy this example domain to src/usr/WWW/ to deploy.
 *
 * Inheritance and the binary-manager glue are copied verbatim from
 * examples/http-app/obj/server.c (the verified working reference); the only
 * difference is the routing surface in dispatch() and the inclusion of the
 * counter object as the route target.
 *
 * Routes:
 *   GET  /counter                -- return the counter as plain text.
 *   POST /increment-with-failure -- catch the deliberate error from the
 *                                   atomic increment and report it in the
 *                                   response body. The subsequent GET
 *                                   /counter is the rollback evidence.
 *   any other                    -- 404 Not Found.
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

# define COUNTER	"/usr/WWW/counter"

int received;				/* received at least one request */
private HttpRequest pendingRequest;	/* awaiting body */

/*
 * On clone, wire HTTP1_SERVER's create with this_object as the server
 * argument. Master sits idle (sscanf returns 0 for the master's name,
 * which has no clone-number suffix).
 */
static void create()
{
    if (sscanf(object_name(this_object()), "%*s#") != 0) {
	::create(this_object(), OBJECT_PATH(RemoteHttpRequest),
		 OBJECT_PATH(RemoteHttpFields));
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
}

private void dispatch(HttpRequest request, StringBuffer body)
{
    string method, path, replyBody;
    mixed err;
    int n;

    method = request->method();
    path = request->path();

    if (method == "GET" && path == "/counter") {
	n = COUNTER->query();
	replyBody = "counter=" + n + "\n";
	emit(makeResponse(200, "OK", replyBody), replyBody);
	return;
    }

    if (method == "POST" && path == "/increment-with-failure") {
	err = catch(COUNTER->increment_with_failure());
	if (err) {
	    replyBody = "deliberate-failure-fired: " + err + "\n";
	    emit(makeResponse(200, "OK", replyBody), replyBody);
	    return;
	}
	replyBody = "increment_with_failure did not error (demo broken)\n";
	emit(makeResponse(500, "Internal Server Error", replyBody), replyBody);
	return;
    }

    replyBody = "404 Not Found\n";
    emit(makeResponse(404, "Not Found", replyBody), replyBody);
}

/*
 * Override: HTTP request parsed and headers complete. Do NOT call
 * ::receiveRequest -- the inherited Connection1::receiveRequest dispatches
 * to relay (= this_object() here, since this_object was passed as the
 * server arg to ::create). Chaining up would infinite-loop. We ARE the
 * relay's target; handle directly.
 *
 * For methods that carry a request body, call expectEntity(length) to
 * switch the connection to MODE_RAW for Content-Length bytes; the
 * platform does not opt in on the subclass's behalf. The body arrives
 * via receiveEntity below.
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
 * Binary-manager glue replicated from /usr/HTTP/api/obj/server1 (which is
 * under /obj/ and not inheritable). These methods connect the platform's
 * user-tier flow contract with the HTTP/1 connection state machine.
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
