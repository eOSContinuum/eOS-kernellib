# Schema

The semantic-vocabulary registry. Schema holds typed element definitions — namespaces, tags, attribute types, child rules, callbacks, iterators — that the marshaler emits against and the XML lexer resolves against. Schemas are akin to DTDs (Document Type Definitions) for XML, but live as live LPC objects: each typed element is a clonable `schema_node` registered with `schema_daemon` by `(namespace, tag)`.

Schema is one of five subsystems in the cohesive Vault layout (Vault / Marshal / Schema / XML / Index). It is format-neutral identity: the schema_node tree describes what elements exist regardless of which marshal format encodes them. Future format bindings (CBOR, Gordian Envelope, triple-store) consume the same Schema registry.

## What this subsystem provides

- **A namespace-indexed registry** of typed elements. Callers resolve `query_node(ns, tag)` → schema_node; the schema_node carries the type, child rules, attribute set, and callback hooks for that element.
- **A type-system dispatcher**. `dtd_daemon` registers type handlers (xml_daemon registers the core XML types; the daemon itself handles the LPC primitives). Callers ask "what's the colour of `lpc_str`?" or "convert this ascii to typed value" and the daemon routes to the handler.
- **The schema-for-schemas tree**. The structural primitives (`Schema:Element`, `Schema:Children`, `Schema:Attribute`, `Schema:Attributes`, `Schema:Callback`, `Schema:Callbacks`, `Schema:Iterator`) are themselves schema_node instances, defined in code by `schema_daemon::configure_initial_nodes()`. The schema_daemon bootstraps its own type tree before any external schema loads.

## Layout

```
src/usr/Schema/
├── initd.c                  compiles dtd_daemon then schema_daemon at boot
├── lib/
│   ├── dtd.c                inheritable: routes type-system queries to dtd_daemon's handlers
│   └── schema_node.c        inheritable: the structural definition of a typed element
├── sys/
│   ├── dtd_daemon.c         registry of data-type definitions and their handlers
│   └── schema_daemon.c      coordinates schema_node by namespace+tag; configures initial nodes
├── obj/
│   └── schema_node.c        thin clonable wrapper over lib/schema_node
└── data/
    └── schema/              on-disk reference schemas (XML format)
        ├── Hierarchy.xml
        ├── UrChild.xml
        ├── UrChildren.xml
        ├── Entry.xml
        └── Entries.xml
```

## Boot sequence

System's initd loads Schema's initd (alphabetical iteration after the `TLS, HTTP, LPC` prefix). Schema's initd:

1. `compile_object("sys/dtd_daemon")` — DTD daemon is the type-handler registry. At `create()` time it inherits `/usr/XML/lib/entities` (cross-domain inherit; works because System's initd grants `set_global_access("XML", TRUE)`).
2. `compile_object("sys/schema_daemon")` — Schema daemon clones the schema_node prototype and calls `configure_initial_nodes()` to code-define the structural primitives.

XML's initd then compiles `xml_daemon`, whose `create()` calls `DTD->register_type(...)` and `DTD->register_colour(...)` against the now-loaded dtd_daemon. The boot order works because System's initd uses a two-pass loop — all platform-layer domain owners (`add_owner`) are registered in pass 1, then each domain's initd loads in pass 2 — so cross-domain inherits resolve regardless of alphabetical-iteration order.

## Bootstrap design: code-defined primitives + XML reference files

The Schema subsystem's structural primitives are bootstrapped in **two places** that must stay consistent:

- **`schema_daemon::configure_initial_nodes()`** — LPC code that clones a schema_node and calls `set_name(ns, tag)`, `add_attribute(...)`, `add_callback(...)` for each primitive. This is the boot-time path; the schema tree comes up before any on-disk schemas load.
- **`data/schema/<Name>.xml`** — XML files describing the same primitives. They are the on-disk reference for the wire format and the load target for the marshaler-driven loader.

The boot path uses the LPC code. `schema_daemon::load_core_schemas()` reads the XML files via `parse_xml + stateimpex::import_state` and cross-checks them against the code-defined primitives; load mismatches surface as boot-log errors. The two definitions must produce equivalent schema_node trees.

## Namespace vocabulary

Schema uses three namespaces for the structural primitives plus their content-domain extensions:

| Namespace | Purpose | Members at boot |
|-----------|---------|-----------------|
| `Schema:` | Schema-for-schemas — the meta-tree describing what schema_nodes look like | `Element`, `Children`, `Attribute`, `Attributes`, `Callback`, `Callbacks`, `Iterator` |
| `Ur:` | Ur-hierarchy primitives — parent/child structural elements applicable to any domain | `Hierarchy`, `Child`, `Children` |
| `Core:` | Property-bag primitives for key-value structured data | `Entry`, `Entries` |

Domain-specific schemas (loaded after boot) introduce their own namespaces. The schema_daemon's `query_node(ns, tag)` resolves any registered `(namespace, tag)` pair regardless of which subsystem registered it.

The vocabulary avoids name collisions with the UR (Uniform Resource) identifier standards published by [Blockchain Commons], since this platform's roadmap composes with those (the `Ur:` namespace here means ur- as in prototype-ancestor, not Uniform Resource): ancestry elements are `Ur:Hierarchy` / `Ur:Child` / `Ur:Children` with a `parent` attribute, and property-bag elements are `Core:Entry` / `Core:Entries` with a `key` attribute.

[Blockchain Commons]: https://github.com/BlockchainCommons

## Boundaries

What this layer deliberately does not provide:

- **HTML output.** The public serialization is XML transport only; `typed_to_html` is a nil-returning passthrough kept for inherit-chain compile parity in dtd_daemon.
- **Rich `LPC_MIXED` conversion.** Ascii conversion currently uses `dumpValue` for serialization and a string-only round-trip for deserialization; a richer type-coercion layer is a future enhancement.
- **Typed-literal parsing in `ascii_to_untyped`.** Only the bare-ASCII fallthrough is supported.
- **Domain-content schemas.** Only the structural primitives ship; applications register their own schemas at boot (see the examples).

## Kernel-layer hooks Schema depends on

Schema uses two cross-cutting infrastructure pieces in eOS-kernellib's kernel layer:

- **`/lib/util/named`** — provides `set_object_name(string)` and `query_object_name()`, wired through `~Index/sys/index_daemon` so the name → object map is global and O(1)-invertible via `find_named()`. Schema's `sys/schema_daemon` and `lib/schema_node` inherit it; daemons and clones set their logical name at create() time (`"Schema:Daemon"`, `"Schema:UrNode"`, `"Schema:<ns>:<tag>"`).
- **`set_global_access("Schema", TRUE)` + `set_global_access("XML", TRUE)`** in `System/initd.c`. Required for cross-domain inherits (Schema → XML/lib/entities, future Marshal → Schema/lib/dtd). The matching two-pass loop in System's domain iteration registers all owners before loading any initd, so the cross-domain inherits resolve at compile time.

## File-by-file reference

### `lib/dtd.c` (123 lines)

Inheritable poor-man's abstract data type repository. Routes type-system queries to dtd_daemon's registered handlers. Inheritors: `xmlgen`, `xmlparse`, `xml_daemon`, `schema_daemon`. Surface includes `queryTypeColour(type)`, `queryColourType(colour)`, `query_ascii_enumeration(type)`, `queryCheckboxed(type)`, `defaultValue(type)`, `typedToAscii(type, val)`, `asciiToTyped(type, ascii)`, `testRawData(type, val)`.

### `sys/dtd_daemon.c` (309 lines)

The daemon. Handlers register types and colours via `register_type(t)` / `register_colour(c)` at their own `create()`; the daemon stores `(type → handler-object)` and dispatches the lib/dtd queries to the right handler. The daemon itself handles the core LPC types (`lpc_str`, `lpc_int`, `lpc_flt`, `lpc_obj`, `lpc_mixed`).

### `lib/schema_node.c` (473 lines)

The structural definition of a typed element. Inheritable; the `obj/schema_node.c` clonable inherits this. Carries `space`, `tag`, `type`, `children`, `attributes`, `iterator`, `callbacks`. Surface: `set_name(ns, tag)`, `query_namespace()`, `query_tag()`, `query_type()`, `add_attribute(id, type, ...)`, `add_callback(method, ...)`, `set_iterator(attr, query)`, `add_child(node)`, `clear_element()`.

### `obj/schema_node.c` (44 lines)

Thin clonable wrapper. Inherits `lib/schema_node`. Each instance is a configured element in the registry. The wrapper detects master-vs-clone via the LPC-idiom `sscanf(object_name(this_object()), "%*s#")` and gives the master the `"Schema:UrNode"` logical name.

### `sys/schema_daemon.c` (314 lines)

The namespace coordinator. Holds `namespaces: mapping (namespace → mapping (tag → schema_node))`. Surface: `register_node(node)`, `query_node(ns, tag)`, `get_node(ns, tag, defaultValue)`, `clear_node(ns, tag)`, `query_namespaces()`. The Node() macro is the convenience for `configure_initial_nodes()`: find-or-clone a `Schema:<ns>:<tag>` and `set_name(ns, tag)` it.

### `initd.c` (27 lines)

Boot trigger. Compiles `sys/dtd_daemon` first, then `sys/schema_daemon`. The daemon `create()`s trigger their own type-handler registration and primitive configuration.

### `data/schema/*.xml` (5 files, ~56 lines total)

On-disk reference for the structural primitives (`Ur:Hierarchy`, `Ur:Child`, `Ur:Children`, `Core:Entry`, `Core:Entries`). Each is a `<object program="/usr/Schema/obj/schema_node">` wrapper around a `<Element ns="..." tag="..." ...>` body. These document the wire format and are the load-target for the future stateimpex-driven `load_core_schemas()` path.

## See also

- [xml.md](xml.md) — the transport layer Schema uses for the on-disk format (`src/usr/XML/`)
- `src/usr/Marshal/` — XmlBinding/stateimpex consumes Schema's `dtd_daemon` for type-handler dispatch during marshal
- `src/usr/Vault/` — uses Schema's namespace registry for per-element marshaling rules; [vault-applications.md](vault-applications.md) walks the application surface
