# Writing HTTP/1 applications

An HTTP/1 application on eOS-kernellib supplies a clonable server object at `/usr/WWW/obj/server`; the kernel layer's HTTP/1 transport binds the binary port and clones that object for every incoming connection. The sections below show what the server object looks like, what it must inherit, how it routes requests, and how an application supports multiple logical apps behind one HTTP/1 port.

**Audience**: an application author building an HTTP/1 service on eOS-kernellib; comfortable with LPC syntax (or read `doc/lpc-essentials.md` first); has the platform running locally per `doc/getting-started.md`.

`doc/architecture.md` covers the capability tiers and the `src/usr/System/sys/http_server.c` bootstrap that makes HTTP routing possible. `doc/application-authoring.md` covers the general tier-E patterns for non-HTTP transports. `doc/runtime-primitives.md` covers the platform properties an HTTP/1 application inherits (atomicity, persistence, hot reload, capability separation).

## The mount point

The HTTP/1 bootstrap (`src/usr/System/sys/http_server.c`) registers itself as the binary-port manager during boot. When a TCP connection arrives on the binary port, the bootstrap's `select()` looks up a clonable object at the kernel-defined path `/usr/WWW/obj/server` via `status(path, O_INDEX)` and, if present, clones it as the per-connection server. The clone receives the connection's I/O and is responsible for the entire HTTP/1 interaction with that client.

An HTTP/1 application is therefore an LPC domain at `/usr/WWW/` that supplies a clonable `obj/server.c`. `add_owner("WWW")` happens automatically when the System initd iterates `/usr/[A-Z]*/initd.c` and finds `/usr/WWW/initd.c`. No further wiring at the kernel layer is required.

## Reference application

`examples/http-app/` carries a working reference implementation: a domain initd and a per-connection server with `GET /health`, `POST /echo`, and a 404 fallback. The code in that directory is the canonical example — accurate, compiling, and runnable. To deploy it:

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

The skeleton inherits two libraries and replicates the binary-manager glue that the kernel's user contract requires. The code below shows only the load-bearing structure; the runnable form is in `examples/http-app/obj/server.c`.

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

`HttpResponse` is constructed with the HTTP version, the status code, and the status phrase; headers are added to an `HttpFields` collection and attached via `setHeaders`:

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

Six methods bridge the platform's user-tier flow contract to the HTTP/1 connection state machine. They are mechanical — replicate them verbatim from the reference application (`examples/http-app/obj/server.c`):

- `login(str)` — called when a connection arrives; calls `::connection(previous_object())`, `flow()`, and `receiveFirstLine(str)`.
- `flow_receive_message(str, mode)` — defers `receiveBytes(str)` via `call_out`.
- `_logout(quit)` — closes the connection and destructs.
- `flow_logout(quit)` — defers `_logout` via `call_out`.
- `flow_message_done()` — defers `messageDone` via `call_out`.
- `timeout()` — closes the connection if no request has been received.

These methods exist because `Server1` (the library form, inheritable) does not carry them; they live in `/usr/HTTP/api/obj/server1.c` (the clonable form, not inheritable). Section "Inherit from `/lib/`, not from `/obj/`" below explains the constraint.

## Platform contracts

Four platform contracts apply to every HTTP/1 application server. The reference application honors all four; an application that omits any of them will fail to boot or fail at first request.

### Inherit from `/lib/`, not from `/obj/`

`Http1Server` is the library form of the HTTP/1 server; `Server1` (under `/obj/`) is the clonable form. Applications inherit the **library** form (`/usr/HTTP/api/lib/Server1`, aliased as `Http1Server`). DGD's `inherit_program` kfun rejects inheritance from a path that does not contain `/lib/` — a discipline that separates inheritable libraries from clonable objects across the kernel layer. The library form of every HTTP/1 component lives under `/usr/HTTP/api/lib/`; the clonable forms under `/usr/HTTP/api/obj/` are not inheritable.

The consequence is that the binary-manager glue in `/usr/HTTP/api/obj/server1.c` cannot be inherited and must be replicated in the application server. The six methods listed above are that replication.

### Inherit `/usr/System/lib/user` for the user-tier flow

The kernel's binary-port manager invokes the connection's `login`, `flow_*`, and `timeout` methods through `LIB_CONN`. `/usr/System/lib/user` provides the user-tier base for those methods and the `::connection(...)` machinery the binary-manager glue uses. Without inheriting this library, the connection state machine cannot progress past initial setup.

### Never call `::receiveRequest`

The HTTP/1 connection library implements `receiveRequest` as a `relay->receiveRequest` callback on a `relay` object. For an application server, the relay **is** the server itself (`this_object()`, passed to `::create` as the first argument). Calling `::receiveRequest` from inside the override would invoke the library's version on the relay, which is the same object, causing unbounded recursion and a stack overflow. Override `receiveRequest`; do not chain to the inherited implementation.

### Call `expectEntity` for body-bearing methods

The HTTP/1 connection library does not read request bodies automatically. After the request line and headers parse, the library calls `receiveRequest` and waits in line-receiving mode until the application either responds (via `sendMessage`) or opts into body receipt via `expectEntity(length)`. Calling `expectEntity(length)` switches the connection to raw-byte mode for exactly `length` bytes, after which the library calls `receiveEntity(chunk)` with the body.

The application is responsible for remembering the request that produced the body. The reference application saves it to a `private HttpRequest pendingRequest` member at the moment `expectEntity` is called, and consumes it in `receiveEntity`.

An application that does not call `expectEntity` for `POST`, `PUT`, or `PATCH` requests will accept the request line and headers but never receive the body; the connection stalls until the client times out.

For chunked transfer encoding (`Transfer-Encoding: chunked`), use `expectChunk` instead of `expectEntity`. WebSocket framing has its own opt-in (`expectWsFrame`); the same pattern applies.

## Multiple applications on one port

The platform mount point is a single path: `/usr/WWW/obj/server`. To run more than one logical application behind one HTTP/1 port, the `/usr/WWW/` server dispatches by route prefix to handlers registered by other domains:

- `/usr/WWW/sys/router.c` — a registry mapping route prefixes to handler objects.
- `/usr/WWW/obj/server.c` — queries the registry from `receiveRequest` and calls the matching handler.
- `/usr/Counter/sys/handler.c`, `/usr/Inventory/sys/handler.c`, etc. — application handlers that register themselves with `/usr/WWW/sys/router` at boot.

This is a higher-level pattern than the single-application reference. The kernel layer is indifferent to which pattern an application chooses; both rely on the same platform contracts.

## Cross-domain initialization order

The System initd compiles `/usr/[A-Z]*/initd.c` in alphabetical order. An application initd that needs to call into another user-layer domain at compile time may run before that domain's initd has compiled. To defer registration until the System initd has finished iterating all domains, use a `call_out` of duration 0:

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

The 0-second `call_out` runs after the System initd commits, which is after every user-layer domain's `create()` has run. Use this pattern for any cross-domain registration where alphabetical compilation order would otherwise leave a dependency unsatisfied.

## Where to next

- `doc/getting-started.md` — install DGD, run the example configuration.
- `doc/architecture.md` — capability tiers, daemons, kernel-layer libraries.
- `doc/application-authoring.md` — general tier-E patterns for non-HTTP transports.
- `doc/runtime-primitives.md` — the platform properties an HTTP/1 application inherits (atomicity, persistence, hot reload, capability separation).
- `examples/http-app/` — runnable reference application.
- `src/usr/System/sys/http_server.c` — the kernel-side bootstrap that mounts `/usr/WWW/obj/server`. Its source comments document the platform-internal asymmetries (the `find_object` vs `status(O_INDEX)` choice, the `inherit_program` `/lib/` constraint).
- `src/usr/HTTP/api/lib/Server1.c` and `src/usr/HTTP/lib/Connection1.c` — the HTTP/1 library implementation that an application server inherits and overrides.
