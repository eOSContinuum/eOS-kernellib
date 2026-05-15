/* SPDX-License-Identifier: BSD-2-Clause-Patent */

/*
 * Reference HTTP/1 application server for eOS-kernellib.
 *
 * Per-connection HTTP server cloned by /usr/System/sys/http_server on every
 * incoming connection to the binary port. The kernel-defined mount point is
 * /usr/WWW/obj/server -- copy this domain to src/usr/WWW/ to deploy.
 *
 * Inherits the HTTP/1 server library (Http1Server, alias for
 * /usr/HTTP/api/lib/Server1) for request parsing and connection state; and
 * the System user-tier connection library (/usr/System/lib/user) for the
 * binary-manager contract. The binary-manager glue (login, flow_*, timeout,
 * _logout) is hand-replicated from /usr/HTTP/api/obj/server1 because DGD's
 * inherit_program requires the inherited path to contain "/lib/" -- the
 * clonable form under /obj/ is not inheritable.
 *
 * Routes:
 *   GET  /health   -- returns 200 OK, body "ok\n".
 *   POST /echo     -- returns 200 OK echoing the request body.
 *   any other      -- returns 404 Not Found.
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
 * Override: HTTP request parsed and headers complete. Do NOT call
 * ::receiveRequest -- the inherited Connection1::receiveRequest dispatches
 * to relay (= this_object() here, since this_object was passed as the
 * server arg to ::create). Chaining up would infinite-loop. We ARE the
 * relay's target; handle directly.
 *
 * For methods that carry a request body, call expectEntity(length) to
 * switch the connection to MODE_RAW for Content-Length bytes; the
 * platform does not opt in on the subclass's behalf. The body arrives
 * via receiveEntity below. Transfer-Encoding: chunked would require
 * expectChunk instead, omitted here for brevity.
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
