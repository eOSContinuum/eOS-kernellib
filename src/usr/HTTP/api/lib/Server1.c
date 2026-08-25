# include <kernel/user.h>
# include <String.h>
# include "HttpRequest.h"
# include "HttpField.h"
# include "HttpResponse.h"
# include <config.h>
# include <version.h>
# include <status.h>

inherit "~/lib/Connection1";


private object server;		/* associated server object */
private string requestPath;	/* HttpRequest object path */
private string headersPath;	/* HttpFields object path */
private HttpRequest request;	/* HTTP request */


/*
 * initialize connection object
 */
static void create(object server, string requestPath, string headersPath)
{
    ::server = server;
    ::requestPath = requestPath;
    ::headersPath = headersPath;
    ::create(server, headersPath);
}

/*
 * default server inactivity timeout
 */
static int inactivityTimeout()
{
    return 60;
}

/*
 * bad request message
 */
static string htmlBadRequest()
{
    return "<HTML>\n<HEAD><TITLE>" + HTTP_BAD_REQUEST +
	   " Bad Request</TITLE></HEAD>\n<BODY><H1>" +
	   HTTP_BAD_REQUEST + " Bad Request</H1></BODY>\n</HTML>\n";
}

/*
 * send bad request response
 */
static void sendBadRequest()
{
    HttpResponse response;
    HttpFields headers;
    string str;
    StringBuffer message;

    response = new HttpResponse(1.1, HTTP_BAD_REQUEST, "Bad Request");
    headers = new HttpFields();
    headers->add(new HttpField("Date", new HttpTime));
    headers->add(new HttpField("Server", ({
	new HttpProduct(SERVER_NAME, SERVER_VERSION),
	new HttpProduct(explode(status(ST_VERSION), " ")...)
    })));
    headers->add(new HttpField("Connection", ({ "close" })));

    str = htmlBadRequest();
    headers->add(new HttpField("Content-Type", "text/html;charset=utf-8"));
    headers->add(new HttpField("Content-Length", strlen(str)));
    response->setHeaders(headers);

    message = new StringBuffer(response->transport());
    message->append(str);
    sendMessage(message);
}

/*
 * parse and verify a HTTP request
 */
static int receiveRequestLine(string str)
{
    request = new_object(requestPath, str);
    return ::receiveRequestLine(request);
}

/*
 * receive request headers
 */
static void receiveHeaders(string str)
{
    int code;

    try {
	code = receiveRequestHeaders(request, new_object(headersPath, str));
    } catch (...) {
	code = HTTP_BAD_REQUEST;
	sendBadRequest();
    }
    receiveRequest(code, request);

    if (code == HTTP_BAD_REQUEST) {
	disconnect();
    }
}

/*
 * finished handling a request
 */
static void _doneRequest(object prev)
{
    if (prev == server) {
	/*
	 * a streaming response that ends by releasing the request
	 * rather than by destructing the connection returns to
	 * ordinary handling here; without this the output-only rule
	 * would outlive the stream and meet the next request on a
	 * persistent connection with a protocol error
	 */
	endStream();
	setMode((persistent()) ? MODE_LINE : MODE_DISCONNECT);
    }
}

/*
 * flow: finished handling a request
 */
void doneRequest()
{
    call_out("_doneRequest", 0, previous_object());
}

/*
 * process login message
 */
static int receiveFirstLine(string str)
{
    int code;

    try {
	code = receiveRequestLine(str);
    } catch (...) {
	code = HTTP_BAD_REQUEST;
	sendMessage(new StringBuffer(htmlBadRequest()));
    }

    if (code != 0 || request->version() < 1.0) {
	/*
	 * call receiveRequest() early, on error output a raw HTML message
	 * and disconnect immediately
	 */
	receiveRequest(code, request);

	if (code == HTTP_BAD_REQUEST) {
	    disconnect();
	}
    }

    startHeaders();
    return MODE_LINE;
}

/*
 * receive a request
 */
static int receiveMessage(string str)
{
    int code;

    if (strlen(str) == 0) {
	return MODE_NOCHANGE;
    }

    try {
	code = receiveRequestLine(str);
	if (request->version() < 1.0) {
	    error("Invalid request version");
	}
    } catch (...) {
	code = HTTP_BAD_REQUEST;
	sendBadRequest();
    }

    if (code != 0) {
	receiveRequest(code, request);

	if (code == HTTP_BAD_REQUEST) {
	    disconnect();
	}
    }

    startHeaders();
    return MODE_LINE;
}

/*
 * send a response
 */
static void _sendResponse(HttpResponse response, object prev)
{
    if (prev == server) {
	::sendResponse(response);
	sendMessage(new StringBuffer(response->transport()), FALSE,
		    response->headerValue("Transfer-Encoding") ||
		    response->headerValue("Content-Length"));
    }
}

/*
 * flow: send a response
 */
void sendResponse(HttpResponse response)
{
    call_out("_sendResponse", 0, response, previous_object());
}

/*
 * send the head of a streaming response
 */
static void _sendStreamResponse(HttpResponse response, object prev)
{
    if (prev == server) {
	::sendResponse(response);
	sendMessage(new StringBuffer(response->transport()));
	beginStream();
    }
}

/*
 * flow: send the head of a streaming response and hold the connection
 * open for pushed frames.
 *
 * The counterpart of sendResponse() for a response that does not end
 * with its head: the server pushes frames with sendChunk() and closes
 * the stream with endChunk(), and the connection is not released by
 * doneRequest() in between. Sending such a head by hand leaves the
 * connection input-blocked for the stream's whole life -- the peer's
 * departure is never seen, and the inactivity backstop disconnects the
 * stream 60 seconds after the request that opened it whatever the
 * stream is doing. Going through here instead re-arms input and
 * suspends that backstop, so the behaviour comes with the platform
 * rather than with each author remembering it.
 *
 * For the stream's life the connection is output-only: a client that
 * sends bytes on it gets a protocol error and a disconnect, because a
 * request queued behind an endless response could never be answered.
 * See Connection1's beginStream().
 */
void sendStreamResponse(HttpResponse response)
{
    call_out("_sendStreamResponse", 0, response, previous_object());
}
