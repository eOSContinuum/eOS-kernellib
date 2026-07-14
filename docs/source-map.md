# Source map

A map of the source tree and a fast index from a subsystem to the code that implements it and the document that explains it. Use this to find where something lives.

For the model behind the layout (why the tiers exist, how the boot sequence reaches a steady state, how objects inherit) read `docs/architecture.md`. For where new code belongs, read `docs/where-code-belongs.md`. For a reading path organized by task, start at `docs/README.md`.

**Audience**: a contributor who knows what they want to change or understand and needs to find the code and its owning document.

## The source tree

Everything the platform ships lives under `src/`, grouped by capability tier. Within any tier the `/lib/`, `/obj/`, `/sys/`, `/data/` subdirectories carry fixed meanings the driver and kernel rely on: inheritable libraries, cloneable masters, singleton daemons, and light-weight value objects. `docs/architecture.md` (Capability tiers, Path conventions) defines both the tier model and these conventions.

### `src/kernel/` (tier B, the kernel layer)

Hard-trusted LPC that mediates the host driver. Hand-edited rarely.

- `sys/` holds the kernel daemons the driver instantiates at boot: `driver.c` (the object DGD calls for login, logout, and error events), `access_daemon.c` (file and command access control), `resource_daemon.c` (per-owner resource tracking), `userd.c` (connection acceptors and their user objects), `capabilityd.c` (the capability store behind the gating surfaces), and `admin_console_registry.c` (the console's extension-verb table).
- `lib/` holds the kernel-tier libraries: `auto.c` (the auto object every compiled object inherits), `admin_console.c` (the console verb set and parser), and `capability.c`, `connection.c`, `user.c`, plus the kfun-exposed APIs under `api/`.
- `obj/` holds the kernel-tier clonables: the console (`admin_console.c`), the connection types (`telnet.c`, `binary.c`, `datagram.c`), and the `user.c` and `rsrc.c` masters.

### `src/lib/`, `src/obj/`, `src/sys/`, `src/include/` (the shared base library)

The inheritable LPC types and utilities available to every tier, below the platform domains.

- `src/lib/` holds the collection and value types: `String.c`, `StringBuffer.c`, the B+ tree family (`BTree.c`, `BTnode.c`) and its persistent subclass pair (`KVstore.c`, `KVnode.c`), `Array.c` (chunked large arrays), the `Time.c` and `GMTime.c` pair, the `Iterator` family, and the `Continuation` family for asynchronous control flow. These are catalogued in `docs/kernel-libraries.md`.
- `src/lib/util/` holds pure-function helpers: `coercion.c` (the canonical value-and-string codec), `properties.c` (the property-layer hook the dispatcher binds to), and `json.c`, `unicode.c`, `base64.c`, `cbor.c`, `cose.c`, `hex.c`, `url.c`, `parse.c`, `ascii.c`, `named.c`, `ur.c`, `random.c`, `file.c`, and others.
- `src/obj/` holds base clonables (`kvnode.c`); `src/sys/` holds the base JSON and UTF-8 codec daemons; `src/include/` holds the shared headers, with the kernel API headers under `src/include/kernel/`.

### `src/usr/System/` (tier C, the platform overlay)

Privileged code owned by `System`, published for global read at boot so every domain inherits the System auto (`lib/auto.c`).

- `sys/` holds the System daemons: `userd.c` (the telnet manager that routes logins), `objectd.c` (the compile-time program graph), `errord.c` (error logging), `upgraded.c` (upgrade-cascade coordination), `portd.c` (the port-label registry over the kernel's numeric port registration), `http_server.c` (the binary-port HTTP manager), `https_server.c` (the HTTPS bootstrap on the labeled TLS port), `identityd.c` (the platform identity registry), `webauthnd.c` (the WebAuthn ceremony daemon), `sessiond.c` (the session-token daemon), `logd.c` (persistent logging), and `persist_helper.c` (statedump support).
- `lib/` holds `auto.c` (the inheritance root for every user-tier object) and `user.c`; `obj/` holds the System login console (`user.c`), the `objectd.c` clonable, and the identity record (`identity.c`).

### `src/usr/<Domain>/` (tier D, the platform domains)

Nine shipped domains, each owned separately and isolated from the others unless granted cross-domain access. Each carries an `initd.c` the System initd compiles at boot.

| Domain | Purpose | Doc |
|---|---|---|
| `HTTP/` | HTTP/1 client and server libraries | `docs/http-applications.md` |
| `TLS/` | TLS 1.3 record and handshake layer (inert unless the host has `KF_SECURE_RANDOM`) | `docs/operations.md` |
| `LPC/` | LPC parser, AST helpers, and an in-LPC compiler (currently unconsumed) | `docs/architecture.md` (Modules) |
| `Merry/` | the Merry script-binding subsystem and the property-change dispatcher | `docs/merry-applications.md`, `docs/merry-language.md`, `docs/dispatcher.md` |
| `Schema/` | the typed-property registry and validator | `docs/schema.md` |
| `Vault/` | structured-object persistence by XML round-trip | `docs/vault-applications.md` |
| `Marshal/` | the property-table-to-XML binding the Vault pipeline uses | `docs/schema.md` |
| `XML/` | XML parse and generate transport | `docs/xml.md` |
| `Index/` | the logical-name registry, name-to-object bindings | `docs/schema.md` |

An application adds its own tier-E domain under `src/usr/<App>/` the same way. See `docs/architecture.md` (Where an application plugs in).

### Outside `src/`

- `examples/` holds the runnable reference applications, one subdirectory each (`http-app`, `vault-app`, `signal-app`, `merry-app`, `chat-app`, `atomic-demo`, `hot-reload-demo`, `hot-reload-master`, `upgrade-cascade`). The harness deploys each into a running platform. See `examples/README.md`.
- `scripts/` holds the boot-and-drive harness: `run-example.sh` (example regressions), `drive-verbs.py` with `verbsets/` (the console driver), and the boot guards.
- `docs/` holds this documentation set; `docs/README.md` is the map.

## Finding a subsystem

Each runtime surface, the code that implements it, and the document that explains it. The code column names an entry point, not every file; follow the owning document for the full surface.

| Subsystem | Entry-point code | Doc |
|---|---|---|
| Capability and authority | `src/kernel/sys/capabilityd.c`, `src/kernel/lib/capability.c`, `src/usr/System/lib/auto.c` | `docs/capability.md`, `docs/architecture.md` |
| Persistence and statedump | the host driver, `src/usr/System/sys/persist_helper.c` | `docs/persistence.md` |
| Property dispatch | `src/usr/Merry/sys/merry.c`, `src/lib/util/properties.c` | `docs/dispatcher.md` |
| Observers | `src/usr/Merry/sys/merry.c` | `docs/observers.md` |
| Schema | `src/usr/Schema/` | `docs/schema.md` |
| Merry language | `src/usr/Merry/` | `docs/merry-language.md` |
| HTTP and transport | `src/usr/HTTP/`, `src/usr/System/sys/http_server.c` | `docs/http-applications.md` |
| TLS | `src/usr/TLS/` | `docs/operations.md` |
| Connections, sessions, users | `src/kernel/sys/userd.c`, `src/usr/System/sys/userd.c`, `src/usr/System/sys/portd.c`, `src/kernel/lib/connection.c` | `docs/architecture.md`, `docs/admin-console.md` |
| Resources and limits | `src/kernel/sys/resource_daemon.c` | `docs/operations.md` |
| Code lifecycle and upgrade | `src/kernel/sys/driver.c`, `src/usr/System/sys/upgraded.c`, `src/usr/System/sys/objectd.c` | `docs/code-lifecycle.md`, `docs/changing-a-running-system.md` |
| Logging and diagnostics | `src/usr/System/sys/logd.c`, `src/usr/System/sys/errord.c` | `docs/operations.md`, `docs/debugging-applications.md` |
| Platform identity substrate | `src/usr/System/sys/identityd.c`, `src/usr/System/obj/identity.c`, `src/include/identityd.h` | `docs/system-daemons.md` |
| WebAuthn ceremonies | `src/lib/util/webauthn.c`, `src/usr/System/sys/webauthnd.c` | `docs/system-daemons.md`, `docs/kernel-libraries.md` |
| Session tokens | `src/usr/System/sys/sessiond.c` | `docs/system-daemons.md` |
| Consoles | `src/kernel/lib/admin_console.c`, `src/usr/System/obj/user.c` | `docs/admin-console.md` |
| Coercion and serialization | `src/lib/util/coercion.c`, `src/usr/Marshal/` | `docs/schema.md` |
| XML | `src/usr/XML/` | `docs/xml.md` |
| Shared LPC libraries | `src/lib/` | `docs/kernel-libraries.md` |

## How the layout is enforced

The tier of a file is its path: `src/kernel/` is tier B, `src/usr/System/` is tier C, and `src/usr/<Domain>/` is tier D. The host driver and the kernel auto read the path to decide what a file may call, so moving a file changes its authority. The `/lib/`, `/obj/`, `/sys/`, `/data/` split inside a tier is structural too (inheritable libraries, cloneable masters, singleton daemons, value objects), and domains load by boot-time discovery of `/usr/[A-Z]*/initd.c`. `docs/architecture.md` (Capability tiers, Path conventions, Boot sequence) carries the mechanics for all three.

Navigation shortcuts: a daemon's name usually matches its file (the resource daemon is `resource_daemon.c`, the upgrade daemon is `upgraded.c`), and a kfun wrapper lives in the kernel auto (`src/kernel/lib/auto.c`) or the System auto (`src/usr/System/lib/auto.c`).

## Where to next

- [`docs/architecture.md`](architecture.md): the tier model, the daemons, the boot sequence, and the auto-inheritance pattern.
- [`docs/where-code-belongs.md`](where-code-belongs.md): where new code goes and the placement shapes for it.
- [`docs/README.md`](README.md): the documentation map and reading paths by task.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md): the contribution flow.
