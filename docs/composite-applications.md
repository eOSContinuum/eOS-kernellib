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
request becomes an authenticated subject.

**Audience**: an application author who has read
[http-applications.md](http-applications.md) and is composing a
multi-domain service; and a platform evaluator tracing how the
authentication substrate ([identity.md](identity.md)) binds to a real
transport.

## Reading the example in stages

The example is large because it composes everything; your first real service should not start by reading all of it. It separates cleanly into three stages, and the walkthrough below follows the same order:

**Stage 1 -- the identity-free core, the skeleton to copy.** A working HTTP service with a persistent daemon, a synchronous audit observer, and a static page, with no identity machinery at all: `WWW/sys/router.c` (the route registry), `WWW/obj/server.c` (the per-connection mount-point server; `tls_server.c` is its HTTPS twin), `Inventory/sys/inventoryd.c` (the daemon -- its plain-mapping store, its dispatched event property, and the audit observer bound to it), `Inventory/sys/demo.c` (the one-route static page), and the `handle()` contract that joins them. The handler's unauthenticated routes (`/inventory/health`, the read-only item and audit lists) complete the shape. This stage is what the harness's module-less profile drives end to end -- transport, routing, and the restore probe, none of which need the crypto module. The audit observer belongs to it as code (identity-free, registered at boot in every profile), but its firing rides an authenticated write, so the module-less phases exercise its registration, not its callback -- the crypto-gated phases do that.

**Stage 2 -- the authd binding.** `Inventory/sys/handler.c`'s ceremony surface: challenge issue and single-use consumption, registration, login, bearer-token validation in front of the mutating routes, and the capability-gated admin route (the check lives in the daemon at the choke-point, not in the handler). Read it against The connection-object-to-daemon seam and Authenticating a wire request below.

**Stage 3 -- the full composition.** The agent self-service surface, the event streams (`Inventory/sys/streamd.c` and the two loopback clients), the recovery ceremony, and the persistence phases that re-drive the whole surface across a restore boot. These are the parts to read when you need each feature, not before.

`sys/test.c` follows the same split: its transport phases exercise stage 1 alone, and the crypto-gated phases add stages 2 and 3 (`examples/composite-app/README.md` maps phases to sentinel counts).

## The demo page: the guided walk

The example has a browser front door: `scripts/demo-composite.sh` is the one-command bring-up (deploy, a locally-trusted certificate, native TLS boot, the two operator verbs), and the page it leaves running at `https://localhost:8443/demo` walks the full identity surface as a numbered sequence -- the unauthenticated entry triad (register / login / recover), a delegable capability whose effect is observable (the capability-gated report), all three authorization tiers refused on purpose along the way, passkey self-service (list, revoke, and add-passkey enrollment, the second-device path), and session-expiry guidance when a bearer call returns 401. The walk is the fastest way to *see* the substrate behave before reading the code; the step-by-step and route table live in `examples/composite-app/README.md` (The browser path), and `docs/common-tasks.md` carries the run recipe.

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

   A handler may append an optional fifth element -- a mapping of
   extra response header name : value pairs -- when a resource needs
   a header beyond the defaults (the demo page sends `Cache-Control:
   no-store` this way). The server stays the only object that touches
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
`register_identity`, `authenticate` (both returning the subject and
a freshly minted session token), `validate`, `logout` -- and never
mints a session for a subject a ceremony did not just prove.
`sessiond->mint(principal)` stays out of tier-E reach by design:
minting for an arbitrary subject string would be authority forgery.
The agent surface extends the same rule: `authenticate_agent_token`
mints only for the ceremony-proven agent identity, and the controller
self-service entries (mint, the own-agents read, suspend/resume,
delegate/undelegate) derive the controlling identity from a live
session, never from the caller. The example binds them under
`/auth/agents` and `/auth/agent-login`, and the demo page's agent
panel drives them from the browser. The recovery ceremony completes
the surface: `/auth/recover` carries a recovery code and a NEW
passkey's registration payload in one request, composed atomically
behind the facade (verify without mint, redeem-and-replace, session
mint), with its challenge issued for that purpose only -- the
handler's single-use store tags every challenge with the route family
it was issued for, the reference discipline for an application running
more than one ceremony kind (`docs/identity.md` Rotation and
recovery).

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
subject is what the domain's authorization decides against -- the
same string the capability store records as a principal.

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

## The event streams

One handler return form extends the one-shot contract: `({ 200, "OK",
"text/event-stream", nil })` tells the mount server to send the head
(chunked transfer encoding, no Content-Length, no Connection: close)
and hold the connection open. From then on the example's broker
(`Inventory/sys/streamd.c`) pushes server-sent-event frames through
the per-connection server clone, each framed by the connection
library's `sendChunk` -- its first in-tree consumer. Three topics
demonstrate the honest event shapes available today:

- `GET /inventory/events` is mutation-driven. The audit observer's
  Merry script routes each event to the broker (the "stream" script
  space) inside the atomic property write, and the broker fans out as
  zero-delay call_outs -- so an aborted mutation rolls its pushes back
  and a stream never carries an event that did not commit.
- `GET /auth/agents/stream?token=<session>` is mutation-driven from
  the identity substrate: the broker subscribes to identityd's
  mutation events (`subscribe_events` / `identity_event`,
  `docs/system-daemons.md`), and on any event that can change a
  controller's own-agents view it recomputes each subscriber's
  snapshot against `authd->query_agents` and pushes the ones that
  changed. The events are armed inside the atomic mutators, so this
  topic carries the same commit-or-nothing property as the audit
  topic: an aborted mutation delivers nothing. Session validity is
  re-checked at each sweep, never on a timer -- so a stream whose
  session expires with no subsequent identity mutation stays open
  until the next event drives a sweep, and closes then; the
  connection-drop path is unchanged. That is the honest price of
  event-driven delivery: no idle work between mutations, and no
  bounded staleness either. The token travels in
  the query string because EventSource cannot set an Authorization
  header; the example README states the tradeoff.
- `GET /inventory/events?heartbeat=1` opts the audit subscriber into
  the timer-driven shape: a tick every ten seconds from the broker's
  self-re-arming call_out, so a live page shows the runtime's
  async-event machinery working continuously between mutations.
  Opt-in keeps the headless driver's stream phases deterministic -- a
  subscriber that did not ask for ticks never receives one.

Being first through the receive side, the driver's streaming client
(`Inventory/obj/stream_client.c`, `expectChunk`) surfaced a latent
defect in the composed connection pattern: the connection library's
internal chunk-line parser shared its name with the relay callback it
invokes, so in a single-object composition every chunked receive
errored. Fixed by renaming the internal function
(`src/usr/HTTP/lib/Connection1.c`).

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
multi-deploy profile form) and runs the driver: 51 sentinels with the
crypto module (ceremonies against the foreign-generated vectors shared
with examples/webauthn-app, the agent lifecycle -- mint, own-agents
list, token ceremony, the ownership and delegability refusals, suspend
and resume -- the event streams: open, observer-driven audit push,
agent-state snapshot and change push, bad-token refusal -- the
identity mutation events: the suspend's event delivered to a
subscribed observer with exact data, the resume's event paired with
its wire response, a refused mutation delivering nothing -- and the
recovery ceremony: self-provisioned codes, the bad-code and
wrong-purpose and never-bare-re-bind refusals, atomic recover onto the
same identity, login with the recovered passkey), 5 in the
transport-only subset without it. Boot 2 restores the snapshot and re-drives the wire: items, a
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
