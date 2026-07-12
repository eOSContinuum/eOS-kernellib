# Writing HTTP/1 applications

An HTTP/1 application on eOS-kernellib supplies a clonable server object at `/usr/WWW/obj/server`. The kernel layer's HTTP/1 transport binds the binary port and clones that object for every incoming connection. The sections below show what the server object looks like, what it must inherit, how it routes requests, and how an application supports multiple logical apps behind one HTTP/1 port.

**Audience**: an application author who is building an HTTP/1 service on eOS-kernellib, is comfortable with LPC syntax (or reads `docs/lpc-essentials.md` first), and has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers and the `src/usr/System/sys/http_server.c` bootstrap that makes HTTP routing possible. `docs/application-authoring.md` covers the general tier-E patterns for non-HTTP transports. `docs/runtime-primitives.md` covers the platform properties an HTTP/1 application inherits (atomicity, persistence, hot reload, capability separation).

## The mount point

The HTTP/1 bootstrap (`src/usr/System/sys/http_server.c`) registers itself as the binary-port manager during boot. When a TCP connection arrives on the binary port, the bootstrap's `select()` looks up a clonable object at the kernel-defined path `/usr/WWW/obj/server` via `status(path, O_INDEX)` and, if present, clones it as the per-connection server. The clone receives the connection's I/O and is responsible for the entire HTTP/1 interaction with that client.

An HTTP/1 application is therefore an LPC domain at `/usr/WWW/` that supplies a clonable `obj/server.c`. `add_owner("WWW")` happens automatically when the System initd iterates `/usr/[A-Z]*/initd.c` and finds `/usr/WWW/initd.c`. No further wiring at the kernel layer is required.

## Reference application

`examples/http-app/` carries a working reference implementation: a domain initd and a per-connection server with `GET /health`, `POST /echo`, and a 404 fallback. The code in that directory is the canonical example: accurate, compiling, and runnable. To deploy it:

```sh
cp -R examples/http-app src/usr/WWW
```

Then run the driver against `example.dgd` and verify with `curl http://localhost:8080/health`.

The sections below explain what the reference application is doing and why. Read the code in `examples/http-app/obj/server.c` alongside this document.

## Application layout

A minimal HTTP/1 application is two files plus an initd:

```text
src/usr/WWW/
  initd.c           — domain initd; compiles obj/server at boot
  obj/
    server.c        — per-connection HTTP/1 server (clonable)
```

The initd compiles the server master at boot, so that the first incoming connection can clone it without racing against compilation:

```c
/* src/usr/WWW/initd.c */
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/server");
}
```

## The server object

The server object is the clonable that the kernel mount point hands every connection. Its job is to receive and parse the HTTP request (the inherited `Http1Server` does this), dispatch the request to application logic, and send the HTTP response.

The skeleton inherits two libraries and replicates the binary-manager glue that the kernel's user contract requires. The code below shows only the load-bearing structure. The runnable form is in `examples/http-app/obj/server.c`.

### Inheritance and clone setup

```c
# include <kernel/user.h>
# include <type.h>
# include <String.h>
# include "/usr/HTTP/api/include/HttpConnection.h"
# include "/usr/HTTP/api/include/HttpRequest.h"
# include "/usr/HTTP/api/include/HttpResponse.h"
# include "/usr/HTTP/api/include/HttpField.h"

inherit Http1Server;                    /* alias for /usr/HTTP/api/lib/Server1 */
inherit "/usr/System/lib/user";

int received;
private HttpRequest pendingRequest;

static void create()
{
    if (sscanf(object_name(this_object()), "%*s#") != 0) {
        ::create(this_object(), OBJECT_PATH(RemoteHttpRequest),
                 OBJECT_PATH(RemoteHttpFields));
    }
}
```

The `create()` clone check is essential: the master sits idle (no clone-number suffix in `object_name`), and only clones call `::create` with arguments. The three positional arguments wire the inherited `Server1` to use this object as the relay (`this_object()`), with the platform-provided request and fields paths.

### Dispatch

`receiveRequest` is the HTTP/1 library callback the application overrides. The library calls it once the request line and headers parse:

```c
static void receiveRequest(int code, HttpRequest request)
{
    string method;
    mixed cl;
    int length;

    received = TRUE;
    if (code != 0 || !request) return;

    method = request->method();
    if (method == "GET" || method == "HEAD" || method == "DELETE") {
        dispatch(request, nil);
        return;
    }

    cl = request->headerValue("Content-Length");
    length = (typeof(cl) == T_INT) ? cl : 0;
    if (length > 0) {
        pendingRequest = request;
        expectEntity(length);            /* body arrives via receiveEntity */
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
```

`dispatch(request, body)` is the application's routing function. It inspects `request->method()` and `request->path()`, builds an `HttpResponse`, and emits it via the inherited `sendMessage`.

### Building a response

`HttpResponse` is constructed with the HTTP version, the status code, and the status phrase. Headers are added to an `HttpFields` collection and attached via `setHeaders`:

```c
HttpResponse response = new HttpResponse(1.1, 200, "OK");
HttpFields headers = new HttpFields();
headers->add(new HttpField("Content-Type", "text/plain; charset=utf-8"));
headers->add(new HttpField("Content-Length", strlen(body)));
headers->add(new HttpField("Connection", "close"));
response->setHeaders(headers);

StringBuffer message = new StringBuffer(response->transport());
message->append(body);
sendMessage(message);
```

### Binary-manager glue

Six methods bridge the platform's user-tier flow contract to the HTTP/1 connection state machine. They are mechanical: replicate them verbatim from the reference application (`examples/http-app/obj/server.c`):

- `login(str)`: called when a connection arrives, calls `::connection(previous_object())`, `flow()`, and `receiveFirstLine(str)`.
- `flow_receive_message(str, mode)`: defers `receiveBytes(str)` via `call_out`.
- `_logout(quit)`: closes the connection and destructs.
- `flow_logout(quit)`: defers `_logout` via `call_out`.
- `flow_message_done()`: defers `messageDone` via `call_out`.
- `timeout()`: closes the connection if no request has been received.

These methods exist because `Server1` (the library form, inheritable) does not carry them. They live in `/usr/HTTP/api/obj/server1.c` (the clonable form, not inheritable). Section "Inherit from `/lib/`, not from `/obj/`" below explains the constraint.

## Platform contracts

Four platform contracts apply to every HTTP/1 application server. The reference application honors all four. An application that omits any of them will fail to boot or fail at first request.

### Inherit from `/lib/`, not from `/obj/`

`Http1Server` is the library form of the HTTP/1 server. `Server1` (under `/obj/`) is the clonable form. Applications inherit the **library** form (`/usr/HTTP/api/lib/Server1`, aliased as `Http1Server`). The driver object's `inherit_program` hook (kernel-layer LPC, `src/kernel/sys/driver.c`) rejects inheritance from a path that does not contain `/lib/`, a discipline that separates inheritable libraries from clonable objects across the kernel layer. The library form of every HTTP/1 component lives under `/usr/HTTP/api/lib/`. The clonable forms under `/usr/HTTP/api/obj/` are not inheritable.

The consequence is that the binary-manager glue in `/usr/HTTP/api/obj/server1.c` cannot be inherited and must be replicated in the application server. The six methods listed above are that replication.

### Inherit `/usr/System/lib/user` for the user-tier flow

The kernel's binary-port manager invokes the connection's `login`, `flow_*`, and `timeout` methods through `LIB_CONN`. `/usr/System/lib/user` provides the user-tier base for those methods and the `::connection(...)` machinery the binary-manager glue uses. Without inheriting this library, the connection state machine cannot progress past initial setup.

### Never call `::receiveRequest`

The HTTP/1 connection library implements `receiveRequest` as a `relay->receiveRequest` callback on a `relay` object. For an application server, the relay **is** the server itself (`this_object()`, passed to `::create` as the first argument). Calling `::receiveRequest` from inside the override would invoke the library's version on the relay, which is the same object, causing unbounded recursion and a stack overflow. Override `receiveRequest`. Do not chain to the inherited implementation.

### Call `expectEntity` for body-bearing methods

The HTTP/1 connection library does not read request bodies automatically. After the request line and headers parse, the library calls `receiveRequest` and waits in line-receiving mode until the application either responds (via `sendMessage`) or opts into body receipt via `expectEntity(length)`. Calling `expectEntity(length)` switches the connection to raw-byte mode for exactly `length` bytes, after which the library calls `receiveEntity(chunk)` with the body.

The application is responsible for remembering the request that produced the body. The reference application saves it to a `private HttpRequest pendingRequest` member at the moment `expectEntity` is called, and consumes it in `receiveEntity`.

An application that does not call `expectEntity` for `POST`, `PUT`, or `PATCH` requests will accept the request line and headers but never receive the body. The connection stalls until the client times out.

For chunked transfer encoding (`Transfer-Encoding: chunked`), use `expectChunk` instead of `expectEntity`. WebSocket framing has its own opt-in (`expectWsFrame`). The same pattern applies.

## Multiple applications on one port

The platform mount point is a single path: `/usr/WWW/obj/server`. To run more than one logical application behind one HTTP/1 port, the `/usr/WWW/` server dispatches by route prefix to handlers registered by other domains:

- `/usr/WWW/sys/router.c`: a registry mapping route prefixes to handler objects.
- `/usr/WWW/obj/server.c`: queries the registry from `receiveRequest` and calls the matching handler.
- `/usr/Counter/sys/handler.c`, `/usr/Inventory/sys/handler.c`, etc.: application handlers that register themselves with `/usr/WWW/sys/router` at boot.

This is a higher-level pattern than the single-application reference. The kernel layer is indifferent to which pattern an application chooses. Both rely on the same platform contracts.

## Cross-domain initialization order

The System initd compiles `/usr/[A-Z]*/initd.c` alphabetically, after a fixed `TLS`, `HTTP`, `LPC` prefix. An application initd that needs to call into another user-layer domain at compile time may run before that domain's initd has compiled. To defer registration until the System initd has finished iterating all domains, use a `call_out` of duration 0:

```c
/* src/usr/Counter/initd.c — registers with the WWW router */
inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    call_out("registerWithWWW", 0);
}

static void registerWithWWW()
{
    /* Safe: all initd iterations have completed. */
    "/usr/WWW/sys/router"->register("/counter", "/usr/Counter/obj/handler");
}
```

The 0-second `call_out` runs after the System initd commits, which is after every user-layer domain's `create()` has run. Use this pattern for any cross-domain registration where compilation order (the `TLS`, `HTTP`, `LPC` prefix followed by the alphabetical remainder) would otherwise leave a dependency unsatisfied.

## API signatures

The signature reference for the HTTP API classes under `src/usr/HTTP/api/lib/`. The canonical-name `#define`s (`HttpRequest`, `Http1Server`, and the rest) come from the headers under `src/usr/HTTP/api/include/`. The source files are authoritative; internal protocol machinery (the parsing state machine in `Connection1.c`) is omitted here.

### `HttpRequest` (`src/usr/HTTP/api/lib/Request.c`) and `HttpResponse` (`src/usr/HTTP/api/lib/Response.c`)

The message value types. Both serialize themselves with `transport()`.

- `create(float version, string method, string scheme, string host, string path)` -- construct a request from components
- `create(float version, int code, string comment)` -- construct a response with a status line
- `void setHeaders(HttpFields headers)` -- attach a headers collection (both classes)
- `mixed headerValue(string str)` -- a header's value by name, or nil (both classes)
- `string transport()` -- the message serialized to HTTP wire format (both classes)
- request accessors: `float version()` / `string method()` / `string scheme()` / `string host()` / `string path()` / `HttpFields headers()`; the request also has `void setHost(string host)`
- response accessors: `float version()` / `int code()` / `string comment()` / `HttpFields headers()`

The `RemoteHttpRequest` / `RemoteHttpResponse` / `RemoteHttpFields` subclasses parse wire input; applications pass their object paths to the server and client constructors and do not use them directly.

### `HttpField` and `HttpFields` (`src/usr/HTTP/api/lib/Field.c`, `Fields.c`)

A single header and the ordered header collection.

- `HttpField`: `create(string name, mixed value, varargs mixed *params)`; `void add(mixed *value, varargs mixed *params)` -- append to an array-valued field (errors otherwise); `int listContains(string str)` -- search a comma-separated value list (the stored value is lowercased for the comparison; pass `str` in lowercase); `string transport()`; accessors `string name()` / `string lcName()` / `mixed value()` / `mixed *params()`
- `HttpFields`: `create()`; `void add(HttpField field)` -- errors on a duplicate singular field, merges array-valued ones; `void del(HttpField field)`; `HttpField get(string name)` -- case-insensitive; `string transport()`; inherits `Iterable`. The `addField` / `addFieldList` create-and-add conveniences are `static` (reachable from inheritors, not on an external `HttpFields` reference); external callers build with `add(new HttpField(...))`

### `Http1Server` (`src/usr/HTTP/api/lib/Server1.c`)

The inheritable per-connection server library; the mount-point object inherits it beside `/usr/System/lib/user` (see The server object above).

- `create(object server, string requestPath, string headersPath)` -- bind the relay object (normally `this_object()`) and the wire-parsing classes (`OBJECT_PATH(RemoteHttpRequest)`, `OBJECT_PATH(RemoteHttpFields)`)
- the application overrides: `void receiveRequest(int code, HttpRequest request)` -- REQUIRED; nonzero `code` is a protocol-level error status, and the override must not chain to the inherited implementation; `void receiveEntity(StringBuffer chunk)` -- the request body, called only after `expectEntity`; `int inactivityTimeout()` -- idle seconds before disconnect (default 60)
- the application calls: `void expectEntity(int length)` -- ask for a body of `length` bytes; `void expectChunk(varargs string compression)` -- opt into chunked transfer (`"gzip"`, `"deflate"`, or nil; decompression requires the host driver's gunzip/inflate kfuns, and without them chunks arrive still compressed); `void sendResponse(HttpResponse response)` -- serialize and send a response, detecting `Transfer-Encoding` / `Content-Length` to hold the connection for a body; `void sendMessage(StringBuffer chunk, varargs int quiet, int hold)` -- send raw message data (`hold` buffers for batching); `void doneRequest()` -- finish the exchange (the connection persists or closes per HTTP/1.1 negotiation)

### `Http1Client` (`src/usr/HTTP/api/lib/Client1.c`)

The inheritable HTTP/1 client library. No shipped example exercises it yet; the shape below is the source contract.

- `create(object client, string host, int port, string responsePath, string headersPath)` -- bind the relay, name the wire-parsing classes, and connect
- the application overrides: `void receiveResponse(HttpResponse response)` -- REQUIRED, the parsed response; as with the server's `receiveRequest`, the override must not chain to the inherited implementation (the same relay dispatch would recurse); `void connected()` / `void connectFailed(int errorcode)` -- connection outcome callbacks; `int inactivityTimeout()` -- default 120
- the application calls: `void sendRequest(HttpRequest request)`; `void expectEntity(int length)` and `void expectChunk(varargs string compression)` -- receive the response body; `void doneResponse()` -- finish the exchange

### `Http1TlsServer` and `Http1TlsClient` (`src/usr/HTTP/api/lib/TlsServer1.c`, `TlsClient1.c`)

TLS-wrapped variants; each dual-inherits its plain-HTTP class plus a buffering connection layer, so the `Http1Server` / `Http1Client` override and call surfaces above still apply. Existence-and-shape:

- `Http1TlsServer`: `create(object server, string certificate, string key, string requestPath, string fieldsPath, string tlsServerSessionPath)` -- certificate and key are PEM strings, the last argument is the object path of the TLS server-session class to instantiate; `tlsAccept(string str, varargs int reqCert, string hosts...)` -- begin the handshake (`hosts` filters SNI; `reqCert` is accepted but currently a no-op, as client-certificate requests are unimplemented in the shipped TLS session); `tlsReceive(string str)`; `tlsClose(int quit)`; `sendMessage(StringBuffer str, varargs int quiet, int hold)` -- encrypt and send (`hold` buffers for batching); `string host()` -- the SNI hostname
- `Http1TlsClient`: `create(object client, string address, int port, string responsePath, string fieldsPath, string tlsClientSessionPath)`; `tlsConnect(varargs string host)` -- handshake, optional SNI hostname; `tlsReceive(string str)`; `tlsClose(int quit)`; `sendMessage(StringBuffer str, varargs int quiet, int hold)`

The TLS layer is inert unless the host driver provides secure randomness (see `docs/operations.md` on `KF_SECURE_RANDOM`).

### `Connection1` flow surface (`src/usr/HTTP/lib/Connection1.c`)

The HTTP/1.x protocol base both server and client inherit. Beyond the functions surfaced above, the flow controls an application may reach:

- `void expectWsFrame()` -- switch to WebSocket frame receipt
- `void sendChunk(StringBuffer chunk, varargs string *params)` / `void endChunk(varargs string *params, HttpFields trailers)` -- emit chunked transfer encoding
- `void sendWsChunk(int opcode, int flags, varargs int mask, StringBuffer chunk)` -- emit a WebSocket frame
- `void terminate()` -- break the connection
- `int persistent()` / `int webSocket()` -- negotiated-state accessors

## Where to next

- [`docs/getting-started.md`](getting-started.md): install DGD, run the example configuration.
- [`docs/architecture.md`](architecture.md): capability tiers, daemons, kernel-layer libraries.
- [`docs/application-authoring.md`](application-authoring.md): general tier-E patterns for non-HTTP transports.
- [`docs/runtime-primitives.md`](runtime-primitives.md): the platform properties an HTTP/1 application inherits (atomicity, persistence, hot reload, capability separation).
- [`examples/http-app/`](../examples/http-app/): runnable reference application.
- [`src/usr/System/sys/http_server.c`](../src/usr/System/sys/http_server.c): the kernel-side bootstrap that mounts `/usr/WWW/obj/server`. Its source comments document the platform-internal asymmetries (the `find_object` vs `status(O_INDEX)` choice, the `inherit_program` `/lib/` constraint).
- [`src/usr/HTTP/api/lib/Server1.c`](../src/usr/HTTP/api/lib/Server1.c) and [`src/usr/HTTP/lib/Connection1.c`](../src/usr/HTTP/lib/Connection1.c): the HTTP/1 library implementation that an application server inherits and overrides.
