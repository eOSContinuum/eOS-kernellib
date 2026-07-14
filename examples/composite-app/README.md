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
  obj/client.c            loopback HTTP/1 client (the client library's
                          first in-tree consumer)
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
| `GET /inventory/items` | none | the persistent core, read side |
| `POST /inventory/items` | bearer | authenticated mutation + audit observer |
| `PUT /inventory/items/<id>` | bearer | application-tier authorization (creator only) |
| `DELETE /inventory/items` | bearer + capability | platform capability `example:inventory-admin` via `is_allowed` |
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
    EXPECTED_OK=19 scripts/run-example.sh composite-app
```

Boot 1 runs the sixteen wire-level phases and dumps a snapshot; boot 2
restores it and proves items, a pre-restore session token, and the
observer binding all survived (the sentinel comment block in
`Inventory/sys/test.c` is the phase-by-phase map).

## The browser path

`WWW/obj/tls_server.c` mounts the same registry behind the labeled
`https` port, and `GET /demo` serves a page that drives the full flow
-- register, login, authenticated create, audit read, capability-gate
refusal -- with a real authenticator. The headless profile verifies the
ceremonies against foreign-generated vectors instead; neither replaces
the other. To run the browser session:

```sh
# deploy both parts
cp -R examples/composite-app/WWW src/usr/WWW
cp -R examples/composite-app/Inventory src/usr/Inventory

# a certificate the browser genuinely trusts (a clicked-through warning
# is not a secure context, and WebAuthn refuses); mkcert is the easy way
mkdir -p src/usr/System/data/tls
mkcert -cert-file src/usr/System/data/tls/cert.pem        -key-file  src/usr/System/data/tls/key.pem  localhost 127.0.0.1

# config: second binary port 8443 (the "https" label) + crypto module,
# as scripts/https-smoke.sh generates it; then boot and open
#   https://localhost:8443/demo
```

The origin must match webauthnd's relying-party configuration (default
`https://localhost:8443`, rpId `localhost`); a different host or port
needs the `webauthn` console verb first. The self-exiting test driver
(sys/test.c) dumps and stops the boot after its phases run -- for an
interactive session, remove `sys/test.c` from the deployed copy and its
`compile_object` line from the deployed initd.
