# composite-app -- a transport-connected composite application

The example above the single-primitive ladder: several platform
primitives composed behind a real transport. An HTTP client on the
platform's binary port reaches, in one request path: the kernel
acceptor, the per-connection WWW server, the route registry, an
application handler in another domain, the platform's identity
substrate (WebAuthn registration and login, bearer sessions), a
persistent daemon, a synchronous Merry audit observer, and the
capability store's `is_allowed` choke-point.

Two domains deploy together (the run-example.sh profile handles this):

```text
WWW/                      the transport-owning domain
  initd.c                 compiles registry + both mount-point servers
  sys/router.c            route-prefix -> handler-path registry
  obj/server.c            HTTP/1 mount point; dispatches via the registry
  obj/tls_server.c        HTTPS mount point, same dispatch (not profiled;
                          needs certificates -- docs/operations.md)
Inventory/                the application domain
  initd.c                 compiles the domain; registers /inventory and
                          /auth with the registry (call_out-0 idiom)
  sys/inventoryd.c        persistent core; Merry audit observer;
                          capability-gated wipe()
  sys/handler.c           routes, WebAuthn/session wire binding,
                          single-use challenge store
  sys/streamd.c           SSE broker: observer-driven audit topic,
                          poll-bridged agent-state topic
  obj/client.c            loopback HTTP/1 client (the client library's
                          first in-tree consumer)
  obj/stream_client.c     loopback SSE client (first through the
                          receive-side chunk surface)
  sys/test.c              boot-time driver: every phase over real TCP
  sys/vectors.h           foreign-generated WebAuthn vectors
                          (scripts/gen-webauthn-vectors.py output,
                          shared with examples/webauthn-app)
```

This directory grounds the "Multiple applications on one port" pattern
in docs/http-applications.md: `WWW/sys/router.c` and the registering
initd are the files that section sketches. The seam walkthrough --
how a connection object reaches a persistent daemon, and how a wire
request becomes an authenticated principal -- is the companion doc
(docs/composite-applications.md).

## Routes

| Route | Auth | What it exercises |
|---|---|---|
| `GET /inventory/health` | none | transport -> router -> handler chain |
| `GET /auth/challenge` | none | WebAuthn challenge via the authd facade |
| `POST /auth/register` | none | TOFU registration; mints identity + session |
| `POST /auth/login` | none | assertion ceremony; mints a session |
| `POST /auth/logout` | bearer | session revocation |
| `POST /auth/agent-login` | none | agent-token ceremony; mints an agent session |
| `GET /auth/recover-challenge` | none | recovery-purpose challenge (the store tags purposes) |
| `POST /auth/recover` | none | recovery ceremony: code + new-passkey attestation, atomic redeem-and-replace |
| `POST /auth/recovery-codes` | bearer | provision own recovery codes (plaintext returned once) |
| `GET /auth/passkeys` | bearer | the session identity's own passkeys (bookkeeping, no key material) |
| `POST /auth/passkeys/<id>/revoke` | bearer | revoke one own passkey; the last one refuses |
| `GET /auth/agents` | bearer | the controller's own-agents view |
| `POST /auth/agents` | bearer | mint an agent; the response carries the token's only plaintext |
| `POST /auth/agents/<uuid>/suspend` | bearer | suspend an own agent; revokes its live sessions |
| `POST /auth/agents/<uuid>/resume` | bearer | resume an own agent (restores authentication only) |
| `POST /auth/agents/<uuid>/delegate` | bearer | delegate an own capability to an own agent |
| `POST /auth/agents/<uuid>/undelegate` | bearer | withdraw the delegation |
| `GET /inventory/events` | none | SSE stream: observer-driven audit events |
| `GET /auth/agents/stream?token=<session>` | query token | SSE stream: own-agents snapshots on change |
| `GET /inventory/items` | none | the persistent core, read side |
| `POST /inventory/items` | bearer | authenticated mutation + audit observer |
| `PUT /inventory/items/<id>` | bearer | application-tier authorization (creator only) |
| `DELETE /inventory/items` | bearer + capability | platform capability `example:inventory-admin` via `is_allowed` |
| `GET /inventory/report` | bearer + capability | the delegable capability `example:delegation-demo` via the same `is_allowed` choke-point |
| `GET /inventory/audit` | none | the observer-written audit trail |
| `GET /demo` | none | the browser demo page (data/demo.html) |

WebAuthn binary fields travel base64url in JSON bodies; sessions are
`Authorization: Bearer <token>`. The wipe route refuses until an
operator grants the capability on the console: `identity grant <uuid>
example:inventory-admin` (docs/identity.md The three-layer
authorization split).

## Verify

```sh
DGD_BIN=/path/to/dgd scripts/run-example.sh composite-app
```

Without the crypto module the ceremony surface stands down and the
transport-only subset runs (5 sentinels). With it, the full set:

```sh
DGD_BIN=/path/to/dgd LPC_EXT_CRYPTO=/path/to/crypto.<ext> \
    EXPECTED_OK=38 scripts/run-example.sh composite-app
```

Boot 1 runs the thirty-six wire-level phases -- the agent lifecycle
(mint, own-agents list, token ceremony, the not-own and not-delegable
refusals, suspend-revokes-sessions, resume-restores-authentication),
the event streams (open, observer-driven audit push, agent-state
snapshot and change push, bad-token refusal), and the recovery
ceremony (self-provisioned codes, the bad-code, wrong-purpose, and
never-bare-re-bind refusals, atomic recover, login with the recovered
passkey) -- and dumps a snapshot; boot 2
restores it and proves items, a pre-restore session token, and the
observer binding all survived (the sentinel comment block in
`Inventory/sys/test.c` is the phase-by-phase map).

## The browser path

`WWW/obj/tls_server.c` mounts the same registry behind the labeled
`https` port, and `GET /demo` serves a page that drives the full flow
-- register, login, authenticated create, audit read, capability-gate
refusal, agent management and delegation, auto-established live
streams with a server heartbeat, and recovery -- with a real
authenticator, numbered steps, and a session-state banner that always
always names who is acting. The headless profile verifies the
ceremonies against foreign-generated vectors instead; neither replaces
the other. To run the browser session:

```sh
# deploy both parts, plus the demo-only System-tier provisioner
cp -R examples/composite-app/WWW src/usr/WWW
cp -R examples/composite-app/Inventory src/usr/Inventory
cp examples/composite-app/System/demo_provisiond.c src/usr/System/sys/

# a certificate the browser genuinely trusts (a clicked-through warning
# is not a secure context, and WebAuthn refuses); mkcert is the easy way
mkdir -p src/usr/System/data/tls
mkcert -cert-file src/usr/System/data/tls/cert.pem        -key-file  src/usr/System/data/tls/key.pem  localhost 127.0.0.1

# config: second binary port 8443 (the "https" label) + crypto module,
# as scripts/https-smoke.sh generates it; then boot
```

The self-exiting test driver (sys/test.c) dumps and stops the boot
after its phases run -- for an interactive session, remove `sys/test.c`
from the deployed copy and its `compile_object` line from the deployed
initd.

After boot, two operator verbs on the admin console
(`docs/admin-console.md` Connecting) pre-provision the delegation walk
-- compile the provisioner and flag the demo capability delegable:

```text
compile /usr/System/sys/demo_provisiond.c
capability delegable example:delegation-demo on
```

then open `https://localhost:8443/demo`. The provisioner is the demo's
stand-in for the operator's grant verb: it grants
`example:delegation-demo` to each identity the page registers, so the
Delegate step completes browser-only, while `example:inventory-admin`
stays ungranted and the admin-wipe 403 keeps teaching the same
boundary. It is demo-only: deployed by this recipe alone, part of no
headless profile -- remove the copied file (and the deployed mounts)
at teardown.

The origin must match webauthnd's relying-party configuration (default
`https://localhost:8443`, rpId `localhost`); a different host or port
needs the `webauthn` console verb first.

### Agents from the browser

Steps 7-14 are the agent surface. The page reserves "principal" for
the human party agents act on behalf of (the API's routes and console
verbs call the principal the agent's "controller"). Management (7-12)
is authd's controller self-service, driven by the logged-in passkey
session; the banner names who is acting, and principal-only steps
disable under an agent session -- except mint (7), which stays
enabled as the standing lesson: under an agent session it refuses
("an agent's principal must be a human identity"), because
delegation grants capabilities, never the principal's standing.
Mint an agent (7): the response is the only time the token plaintext
exists, and the page fills it into the agent-login field -- copy it
now or lose it. List (8) shows each of your agents with its suspension
state and delegated capabilities. Suspend/Resume (9/10) and
Delegate/Undelegate (11/12) act on the uuid field (mint and list fill
it). Delegate succeeds out of the box for `example:delegation-demo` --
the provisioner granted it to the principal, and the bring-up
flagged it delegable. Any other capability shows the refusal an
operator has not enabled; `example:inventory-admin` delegates only
after the console runs the real verbs:

```text
identity grant <controller-uuid> example:inventory-admin
capability delegable example:inventory-admin on
```

Agent login (13) trades the minted token for an agent session: the
page's bearer session becomes the agent's, so an item create runs as
the agent and lands in the audit trail under the agent's identity
string; updating the principal's item as the agent refuses (the
application-tier creator check, the page's 3b), and the admin wipe
(6) stays 403 unless an operator ran the `example:inventory-admin`
verbs above and you delegated it to the logged-in agent. The report (14) makes the delegation observable:
gated by `example:delegation-demo` at the same `is_allowed`
choke-point as the wipe, it answers 200 for the principal from
registration, 403 for the agent until a delegation stands, 200 while
it does, and 403 again after undelegate. Passkey login (1b) switches
the page back to the principal; suspending the agent then revokes its
sessions and refuses its ceremony until resume.

The event streams open on their own: the audit stream (with the
server's ten-second heartbeat tick) when the page loads, the agent
stream with each principal session. The banner shows both, and the
heartbeat line in the log updates in place while you work -- suspend
or delegate in another tab and watch the agent snapshot arrive. The
agent stream carries the session token in its URL because EventSource
cannot set headers; that keeps the demo dependency-free, and a
production deployment would prefer a cookie-bound session so tokens
stay out of request logs.

### Recovery from the browser

Recovery is one of the page's three ways in: the entry triad -- 1a
Register, 1b Login, 1c Recover -- is picked by what you hold
(nothing, a passkey on this device, or a recovery kit), so a
returning user on a new device is never funneled into Register
forking a fresh identity. The stored uuid plus one code bind the new
device's passkey to the SAME identity; the old passkey keeps working
(two devices, one identity), and after a lost device, revocation is
the operator plane's half. Recovery is also the human flow, and the
page enforces it: kit minting disables under an agent session, and
Recover refuses the agent uuid with the principal uuid to use
instead. Mint recovery codes (15) while logged in as the principal:
the response is the only time the plaintext exists, and the log
prints the kit (uuid + codes) to save as one unit. To recover after
losing the passkey (simulate by reloading the page, which drops the
session), enter the uuid and one code, then Recover (1c): the page
fetches a
recovery-purpose challenge, runs a fresh authenticator registration
ceremony, and sends code and attestation in one request. The platform
redeems the code and binds the new passkey atomically to the SAME
identity -- the identity string in the log matches the one you
registered --
and the code is spent: a second recover with it refuses. The old
passkey keeps working until revoked -- which is now the walk's last
step: list your passkeys (16, bookkeeping only, the current one
marked) and revoke the lost device's (17). The facade refuses your
last passkey, so a single-passkey identity answers 403 until 1c binds
a second; the operator-plane `identity revoke` verb remains for
records an operator manages.
