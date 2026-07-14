# Writing composite applications

The single-primitive examples each prove one thing; a real service
composes several behind a transport. This page walks
`examples/composite-app/` -- a two-domain application in which an HTTP
request crosses, in one path: the kernel acceptor, a per-connection
server clone, a route registry, an application handler in another
domain, the platform's identity substrate, a persistent daemon with a
synchronous audit observer, and the capability store's choke-point. It
is the runnable form of the multi-application router sketched in
[http-applications.md](http-applications.md) "Multiple applications on
one port", and the reference for the seams that sketch leaves open:
how a connection object reaches a persistent daemon, and how a wire
request becomes an authenticated principal.

**Audience**: an application author who has read
[http-applications.md](http-applications.md) and is composing a
multi-domain service; and a platform evaluator tracing how the
authentication substrate ([identity.md](identity.md)) binds to a real
transport.

## The shape

```text
/usr/WWW/        owns the transport: mount-point servers + route registry
/usr/Inventory/  owns the application: handler, daemon, observer
```

The WWW domain carries no application logic. Its per-connection
servers (`obj/server.c` for the binary port, `obj/tls_server.c` for
the labeled https port) resolve each request path through
`sys/router.c` -- a registry mapping route prefixes to handler object
PATHS -- and relay to the owning domain. Paths, not object references:
a handler recompile or upgrade never leaves a stale reference in the
registry, because the server re-resolves with `find_object` per
request.

The application domain registers its prefixes at boot from its own
initd, deferred one `call_out` tick because the System initd compiles
domain initds alphabetically and `/usr/WWW` may not exist yet when
`/usr/Inventory`'s `create()` runs (the cross-domain initialization
order of [http-applications.md](http-applications.md)). One domain may
own several prefixes: Inventory registers both `/inventory` and
`/auth`.

## The connection-object-to-daemon seam

Connection objects are ephemeral; domain state is not. The per-request
path is:

1. The kernel clones the mount-point server for the connection
   (`docs/http-applications.md` The mount point).
2. The clone parses the request (inherited `Http1Server`), resolves
   the path through the registry, and relays on the narrow handler
   contract:

   ```c
   mixed *handle(string method, string path, string body,
                 string authorization)
       -> ({ code, phrase, contentType, body })
   ```

   The server stays the only object that touches
   HttpRequest/HttpResponse wire objects; handlers see strings and
   return strings. A handler error becomes a 500 without breaking the
   connection contract.
3. The handler calls into the domain daemon
   (`Inventory/sys/inventoryd.c`), which owns the durable state. The
   connection clone dies with the connection; the daemon's items
   mapping, its observer binding, and the identity substrate's records
   persist orthogonally and survive restore -- the example's boot-2
   phases prove all three over the wire.

Policy lives with the state, not the transport: inventoryd decides who
may update an item (application-tier authorization, against the item's
recorded creator) and whether a wipe is allowed (the platform
capability, below). The handler only translates decisions to status
codes.

## Authenticating a wire request

The identity substrate's daemons gate every entry to System/kernel
callers, so a tier-E transport cannot call them directly. The seam is
`src/usr/System/sys/authd.c`, the transport authentication facade: it
exposes exactly the ceremony-plus-session flow -- `issue_challenge`,
`register_identity`, `authenticate` (both returning the principal and
a freshly minted session token), `validate`, `logout` -- and never
mints a session for a principal a ceremony did not just prove.
`sessiond->mint(principal)` stays out of tier-E reach by design:
minting for an arbitrary principal string would be authority forgery.

**Challenge ownership is the application's.** webauthnd holds no
challenge state (the caller that issued a challenge owns it --
[system-daemons.md](system-daemons.md)), and authd preserves that
contract downward. The handler's single-use store is the reference
discipline: `GET /auth/challenge` issues through authd and records the
value; a ceremony payload must present a challenge this handler issued
and not yet consumed, so a replayed ceremony dies in the handler
before any cryptography runs. The composite driver proves both the
consume (CHALLENGE-REPLAY-REFUSED) and the deeper webauthnd negatives
(bad origin, signature-counter replay) over the wire.

**Sessions bind as bearer tokens.** The HTTP layer parses an
`Authorization` header into a value object
(`src/usr/HTTP/api/lib/Authentication.c`) without enforcing anything;
the WWW server re-serializes it to its wire form, and the handler
parses `Bearer <token>` and validates through authd. The validated
principal is what the domain's authorization decides against.

The [identity.md](identity.md) three-layer split, played out on real
routes:

| Layer | Route | Decision |
|---|---|---|
| Application-tier | `PUT /inventory/items/<id>` | inventoryd: creator only |
| Platform capability | `DELETE /inventory/items` | inventoryd asks `is_allowed("example:inventory-admin", principal)`; the grant is operator console work (`identity grant`) |
| Held-handle mediation | -- | deferred platform-wide ([capability.md](capability.md) Identity principals and the operator grant path) |

## The audit observer

inventoryd is a property host (one inherit, `/lib/util/properties`),
and every mutation writes an event property inside the same `atomic`
function as the mutation. A Merry observer registered on that property
appends the event to the audit-log property synchronously, inside the
write ([signal-applications.md](signal-applications.md)): mutation and
audit commit or roll back together, with no queue and no reconciliation
job. `GET /inventory/audit` reads the trail back over the wire, and the
restore boot proves the binding itself persists.

## The outbound client seam

The example's test driver reaches every phase over real TCP through
`Inventory/obj/client.c` -- the first in-tree consumer of the HTTP/1
client library ([application-authoring.md](application-authoring.md)
Outbound connections). Being first, it surfaced three latent defects
in the plain-client path, all documented in the client's header:
`obj/client1.c` double-connects; its driver-level `MODE_BLOCK` is
never lifted (only `MODE_UNBLOCK` lifts blocking, and nothing in the
client path sends it), leaving the response unread in the socket; and
driver line-framing races a response whose head and body share one TCP
segment, parsing the body as a status line.

The working shape is the one the TLS variants already use: compose
`Http1Client` with `BufferedConnection1` (now on the API surface at
`/usr/HTTP/api/lib/BufferedConnection1.c`), keep the driver-level
connection in raw mode for the whole exchange, and let the buffered
layer do line and entity framing internally, synchronously with the
connection state machine. An application adopting the outbound surface
should start from `Inventory/obj/client.c`, not `obj/client1.c`.

## Verification

`scripts/run-example.sh composite-app` deploys both domains (the
multi-deploy profile form) and runs the driver: 19 sentinels with the
crypto module (ceremonies against the foreign-generated vectors shared
with examples/webauthn-app), 5 in the transport-only subset without
it. Boot 2 restores the snapshot and re-drives the wire: items, a
pre-restore session token, and the observer binding all survive. The
sentinel comment block in `Inventory/sys/test.c` is the
phase-by-phase map; `examples/composite-app/README.md` carries the
route table and run recipes.

## Where to next

- [`../examples/composite-app/`](../examples/composite-app/): the code
  this page walks.
- [http-applications.md](http-applications.md): the single-application
  reference, the platform contracts, and the API signatures.
- [identity.md](identity.md): the identity substrate this example
  binds to the wire.
- [system-daemons.md](system-daemons.md): authd, identityd, webauthnd,
  and sessiond signatures.
- [signal-applications.md](signal-applications.md): the observer
  primitive the audit trail uses.
