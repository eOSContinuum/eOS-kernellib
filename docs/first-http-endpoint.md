# Your first HTTP endpoint

A hands-on tutorial, continuing from [first-application.md](first-application.md). Your key-value service answers the console; here you put an HTTP face on it: a transport domain whose per-connection server routes requests to the daemon you already wrote. You will drive the service with `curl`, add a route to it while it is running, and restart the process to find the store reachable over HTTP with everything still in it.

**Audience**: a reader who has completed [first-application.md](first-application.md) (the `KV` domain exists, `kv_daemon` compiled, at least the `"lang"` key stored). Every command is shown with its expected output. [http-applications.md](http-applications.md) is the reference this tutorial deliberately stays lighter than; each contract it enforces is named here the first time you meet it.

**What you'll have at the end**: a second domain fronting your first one over real TCP, three HTTP routes you wrote (read, write, health), one route added to the live service without a restart, and the same persistence proof as before -- this time observed through `curl`.

## 1. The transport domain

The platform's HTTP/1 bootstrap clones a server object at one kernel-defined mount point per connection: `/usr/WWW/obj/server`. Serving HTTP therefore means owning the `WWW` domain and putting a server clonable at that path. At the console:

```text
# grant WWW access
# access WWW
WWW has no special access.
```

Your application now spans two domains, and that split is the platform's shape, not an accident: `KV` owns the state and the methods, `WWW` owns the wire. The server you are about to write calls across the boundary with the same `"/usr/KV/sys/kv_daemon"->get(...)` calls you typed at the console -- a cross-domain call needs no grant, because the access model gates files, not calls ([architecture.md](architecture.md) System global access; the capability story for gating *calls* is [capability.md](capability.md)).

## 2. Start from the shipped server

An HTTP/1 server object carries a block of connection glue that is contract, not creativity: the platform's binary-port manager drives `login` / `flow_*` / `timeout` methods, and the HTTP library needs its clone wired at create time. The reference application ships that glue working; start from it rather than retyping it. On the host:

```sh
mkdir -p src/usr/WWW/obj
cp examples/http-app/obj/server.c src/usr/WWW/obj/server.c
```

`src/usr/WWW/initd.c`, the domain bootstrap, same shape as `KV`'s:

```c
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/server");
}
```

Everything above `dispatch()` in the copied server is the glue: the clone wiring in `create()`, the response helpers, the entity plumbing, and the flow methods ([http-applications.md](http-applications.md) Platform contracts explains each; the tutorial only asks you to keep them). One helper deserves your attention before you route anything -- `emit()` ends with:

```c
    /* the response is complete: release the request so the flow layer
     * closes or re-arms the connection and the user slot recycles */
    this_object()->doneRequest();
```

That release is the fifth platform contract: every completed response must hand the connection back, or each request holds one of the platform's connection slots until a timeout reaps it ([http-applications.md](http-applications.md) Release each completed request).

## 3. Route to your daemon

Now replace the whole `dispatch()` function in `src/usr/WWW/obj/server.c` -- delete the shipped one (the `/health`, `/status`, and `/echo` routes) and put this in its place:

```c
private void dispatch(HttpRequest request, StringBuffer body)
{
    string method, path, key, reply;
    mixed value;

    method = request->method();
    path = request->path();

    if (method == "GET" && path == "/health") {
	emit(makeResponse(200, "OK", "ok\n"), "ok\n");
	return;
    }

    if (sscanf(path, "/kv/%s", key) != 0 && strlen(key) != 0) {
	if (method == "GET") {
	    value = "/usr/KV/sys/kv_daemon"->get(key);
	    if (value == nil) {
		emit(makeResponse(404, "Not Found", "no such key\n"),
		     "no such key\n");
	    } else {
		reply = (string) value + "\n";
		emit(makeResponse(200, "OK", reply), reply);
	    }
	    return;
	}
	if (method == "PUT") {
	    "/usr/KV/sys/kv_daemon"->put(key, drainBody(body));
	    emit(makeResponse(200, "OK", "stored\n"), "stored\n");
	    return;
	}
    }

    emit(makeResponse(404, "Not Found", "404 Not Found\n"),
	 "404 Not Found\n");
}
```

Three routes: a health line, a read, a write. The read calls the same `get()` you drove from the console; the write feeds `put()` from the request body, which the glue's `expectEntity` machinery delivered ([http-applications.md](http-applications.md) Call `expectEntity` for body-bearing methods -- the copied `receiveRequest` already opts in for you on any request carrying a `Content-Length`).

## 4. Compile and drive it with curl

At the console, compile the new domain's two files -- the same two-step console form as [first-application.md](first-application.md) section 3 (the console `compile` defers `create()`, so the initd has not compiled the server for you; from the next cold boot it will):

```text
# compile /usr/WWW/initd.c
$1 = </usr/WWW/initd>
# compile /usr/WWW/obj/server.c
$2 = </usr/WWW/obj/server>
```

(As in `first-application.md`: the `$N` indices assume you have run nothing extra at the console. Any additional value-producing command shifts every later slot, so compare the values, not the slot numbers.)

From a second terminal on the host, drive the service over TCP (the binary port from `example.dgd` is 8080):

```sh
curl http://localhost:8080/health
# ok

curl http://localhost:8080/kv/lang
# LPC

curl -X PUT --data-binary 'orthogonal' http://localhost:8080/kv/persistence
# stored

curl http://localhost:8080/kv/persistence
# orthogonal

curl http://localhost:8080/kv/missing
# no such key
```

The second command is the moment this tutorial exists for: `"lang"` went into the store from the console in [first-application.md](first-application.md), and it just came back over HTTP. One state, two surfaces. The daemon neither knows nor cares which transport asked.

## 5. Add a route to the running service

The store can be written and read over the wire, but not emptied. Add deletion -- without stopping anything. In `src/usr/WWW/obj/server.c`, add one more method branch inside the `/kv/` block, after the `PUT` branch:

```c
	if (method == "DELETE") {
	    "/usr/KV/sys/kv_daemon"->remove(key);
	    emit(makeResponse(200, "OK", "removed\n"), "removed\n");
	    return;
	}
```

Recompile the server at the console, then use the new route immediately:

```text
# compile /usr/WWW/obj/server.c
$3 = </usr/WWW/obj/server>
```

```sh
curl -X DELETE http://localhost:8080/kv/persistence
# removed

curl http://localhost:8080/kv/persistence
# no such key
```

The same hot-fix move as [first-application.md](first-application.md) section 5, now on a live network service: the master recompiled in place, and the next connection's clone dispatched to the new program. No listener restarted, no connection dropped, no deploy step ([code-lifecycle.md](code-lifecycle.md)).

## 6. The persistence win, over the wire

Repeat the shutdown from the first tutorial, this time proving the survival through HTTP. At the console:

```text
# reboot
```

Restart from the snapshot -- naming **both** files this time. Your first reboot (in [first-application.md](first-application.md)) wrote a full snapshot; this second one wrote an incremental against it, and an incremental restores only on top of its base ([operations.md](operations.md) Backing up and restoring state maps which stop path leaves which):

```sh
/path/to/dgd/bin/dgd example.dgd state/snapshot state/snapshot.old
```

Then, with no console login at all:

```sh
curl http://localhost:8080/kv/lang
# LPC
```

The process died and came back, and the first HTTP request finds the store intact -- daemon, server, routes, and data all restored from the image. The restored boot did not re-run either initd; there was nothing to rebuild ([persistence.md](persistence.md)).

## What you just used

| Section | Piece | Depth |
|---|---|---|
| 1 | The mount-point convention and the two-domain split | [http-applications.md](http-applications.md) |
| 2 | The platform contracts an HTTP server honors | [http-applications.md](http-applications.md) Platform contracts |
| 3 | Cross-domain calls as the composition seam | [architecture.md](architecture.md) |
| 4 | One state behind two surfaces | [runtime-primitives.md](runtime-primitives.md) |
| 5 | Hot reload on a live network service | [code-lifecycle.md](code-lifecycle.md) |
| 6 | Orthogonal persistence observed through a transport | [persistence.md](persistence.md) |

## Cleaning up

Your domain lives at `src/usr/WWW`, and that mount name collides with the harness: any `scripts/run-example.sh` or `scripts/drive-verbs-smoke.sh` run removes `src/usr/WWW` in its clean-slate step, taking your tutorial domain with it -- copy anything you want to keep before running the example regressions. When you are done, reset the checkout per [common-tasks.md](common-tasks.md) Reset a development checkout to a clean slate; that recipe also names `src/usr/Pet` and `src/usr/KV` from the earlier tutorials, which the harness does not remove on its own.

## Where to next

- **[first-composition.md](first-composition.md)** continues this build directly: a second object kind with a registered name, a secondary index the store can never disagree with, an audit observer inside the atomic write, and a capability-gated admin route -- every mechanism the composite example composes, built at tutorial scale.
- **[http-applications.md](http-applications.md)** is the reference behind every line you copied: the five platform contracts, the API signatures, routing multiple applications on one port, and the pieces this tutorial skipped (chunked transfer, WebSocket framing, TLS variants).
- **[composite-applications.md](composite-applications.md)** is the assembly this tutorial points toward: `examples/composite-app/` composes the same seams you just built -- transport domain, cross-domain handlers, a persistent daemon -- with identity, an audit observer, and event streams. Read it with the composite example beside you.
- **[vault-applications.md](vault-applications.md)** moves your store from image-only persistence to schema-registered on-disk XML, the durability layer a snapshot alone does not give you.
- **`scripts/README.md`** shows how the shipped examples turn exactly this kind of service into boot-time regressions with sentinel drivers.
