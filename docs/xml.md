# XML

The XML transport layer. XML parses ASCII XML to an internal XMD tree, generates ASCII XML from XMD, and registers the core XML types with the Schema subsystem's `dtd_daemon` so callers downstream of marshaling can resolve element / pcdata / samref / bool typed values through a single dispatcher.

XML is one of five subsystems in the cohesive Vault layout (Vault / Marshal / Schema / XML / Index). It is the format-implementation layer: where Schema defines what elements exist (format-neutral), XML defines how to wire-encode them. Future formats (CBOR, Gordian Envelope) would sit alongside as sibling format implementations, each registering its types with `dtd_daemon`.

**Audience**: a kernel or application author reading or extending the XML transport beneath the Vault pipeline; most application authors touch it only through [vault-applications.md](vault-applications.md).

## What this subsystem provides

- **XML parser** — converts ASCII XML to XMD, the internal binary form. All-LPC lexer; slow but flexible, and inheritable so callers can parse inline.
- **XML generator** — emits ASCII XML from XMD, writing to a caller-supplied StringBuffer-shape sink.
- **XMD helpers** — construct, query, and reshape XMD trees built from the LWO data wrappers in `data/`.
- **Type registration** — at create time, `xml_daemon` registers `xml_element`, `xml_pcdata`, `xml_samref`, and `xml_bool` with `dtd_daemon`. Subsequent typed conversions for these types route through xml_daemon's handler methods.

## Layout

```
src/include/
└── XML.h                    type constants (XML_ELEMENT etc.) + XML daemon path

src/usr/XML/
├── initd.c                  compiles the three data/ LWO wrappers, then xml_daemon, at boot
├── include/
│   ├── XMLIn.h              lexer state vars + macros (included by lib/xmlparse)
│   └── XMLOut.h             output-buffer macros (included by lib/xmlgen)
├── lib/
│   ├── entities.c           XML entity quote/unquote (&amp; etc.)
│   ├── xmd.c                XMD tree construction and query helpers
│   ├── xmlgen.c             generate ASCII XML from XMD
│   └── xmlparse.c           parse ASCII XML to XMD
├── sys/
│   └── xml_daemon.c         registers XML types + colours with dtd_daemon
└── data/
    ├── element.c            LWO wrapper for XML element data
    ├── pcdata.c             LWO wrapper for PCDATA
    └── samref.c             LWO wrapper for samref data (structural; no content interpretation)
```

## Boot sequence

System's initd loads XML's initd in alphabetical iteration after Schema (S) — Schema must precede XML so `dtd_daemon` exists when `xml_daemon::create()` calls `DTD->register_type(...)`. XML's initd:

1. `compile_object("data/element")`, `compile_object("data/pcdata")`, `compile_object("data/samref")` — compiles the LWO data wrappers first; `new_object()` does not auto-compile an uncompiled master, so skipping this step would fail `load_core_schemas` with "Cannot create new instance of /usr/XML/data/element".
2. `compile_object("sys/xml_daemon")` — compiles the daemon. At `create()` time it inherits `~/lib/xmlparse`, `~/lib/xmlgen`, `~/lib/xmd` (which transitively pull `lib/entities`), then makes four separate `DTD->register_type(...)` calls (`XML_ELEMENT`, `XML_SAMREF`, `XML_PCDATA`, `XML_BOOL`) and three separate `DTD->register_colour(...)` calls (`COL_ELEMENT`, `COL_SAMREF`, `COL_PCDATA`) against the now-loaded Schema's `dtd_daemon`.

After boot, Schema-mediated type dispatch resolves any XML-typed values through xml_daemon's handler methods (`queryTypeColour`, `typedToAscii`, `asciiToTyped`, `testRawData`, etc.).

## Internal vs wire format

XML uses two representations and a clear vocabulary for the boundary:

- **XML** is the ASCII serialization. The lexer in `xmlparse.c` reads it; the generator in `xmlgen.c` writes it. Public.
- **XMD** is the internal binary form. LPC mappings and arrays carrying parsed element data, anchored by LWO wrappers in `data/`. Internal.

`xmd.c` is named for the internal form; the helpers there (`xmdElts`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdOptimize`, etc.) operate on XMD trees, not on serialized XML strings.

## Sugar tags: structural-only

Sugar tags are a legacy inline content-substitution syntax (`$(ref ...)` references and `{ choice | choice }` alternation embedded in text). Their interpretation is not part of this transport layer. Three consequences:

- **`xmlgen.c::generate_pcdata` does not consult a sugar-tag daemon**; it dispatches element / pcdata / samref directly.
- **The brace form (`{ choice | choice }`) raises LexErr.** `xmlparse.c::p_oneof` does not interpret it; the parser flags it as invalid input.
- **`samref.c` is a structural LWO.** The wrapper carries XML data with no semantic interpretation. xmlgen.c emits a literal `$(ref attrs)` form for any reader that wants it; no content rewrite happens in this layer.

## Boundaries

What this layer deliberately does not provide:

- **Sugar-tag interpretation** — see above.
- **Typed-literal forms in `XML_BOOL`** — the serialization is a simple `true` / `false`.
- **Logging** — the `DEBUG` / `Debug` / `XDebug` diagnostic macros are no-ops; the `dump_value` references in their args are dead code. The logging facility (`logd`, `docs/operations.md`) has since landed; wiring these macros to it is unstarted.

The `SID` and `DTD` daemon constants are inline-defined (`SID = /usr/Schema/sys/schema_daemon`, `DTD = /usr/Schema/sys/dtd_daemon`).

## Naming convention

Function names within the XML transport are camelCase, matching the `/lib/util/*` helper convention:

- `xmdElts`, `xmdText`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdRef`, `xmdRefRef`, `xmdRefAttributes`, `xmdWipeTags`, `xmdStripPcdata`, `xmdForceToData`, `xmdOptimize`, `attributesToMapping`
- `queryColour`, `queryColourValue`, `entityToAscii`, `asciiToEntity`
- `sysLog`, `dumpValue`

The **DTD-callback API** in `xml_daemon.c` is camelCase to match (`queryTypeColour`, `typedToAscii`, `asciiToTyped`, `testRawData`, `queryCheckboxed`, `defaultValue`), apart from two snake_case accessors, `ascii_size` and `ascii_height`, which sit outside the handler dispatch (the dispatch's size and height probes ask handlers for `queryAsciiSize` / `queryAsciiHeight`). The camelCase handler set is called by name through `dtd_daemon`'s dispatch, so all handlers registered with the daemon must agree on those names; any new handler implements the camelCase set.

Two boundaries retain `snake_case`: the schema_node query surface (`query_attributes`, `query_name`, and kin -- the marshaler walks these on schema definitions, a separate contract from handler dispatch), and `lower_case` / `strip` / `strip_left` / `strip_right` in `/lib/util/ascii`.

## File-by-file reference

### `src/include/XML.h` (14 lines)

Public type constants: `XML_ELEMENT`, `COL_ELEMENT`, `XML_SAMREF`, `COL_SAMREF`, `XML_PCDATA`, `COL_PCDATA`, `XML_MIXED`, `XML_BOOL`, plus the `XML` constant pointing at `/usr/XML/sys/xml_daemon`. Included by callers that need to reference XML types by name.

### `src/usr/XML/include/XMLIn.h` (302 lines)

Lexer state variables and macros for `lib/xmlparse.c`. `DEBUG(...)` macro calls expand to no-ops in the current logging story; the `dump_value` references inside those calls are dead code (never compiled). The logging facility (`logd`, `docs/operations.md`) has since landed; wiring `DEBUG` and `dump_value` to it is unstarted.

### `src/usr/XML/include/XMLOut.h` (86 lines)

Output-buffer macros for `lib/xmlgen.c`. Operates against the StringBuffer-shape `append(string)` sink xml_daemon constructs (via `new StringBuffer()`, not `clone_object`) per gen_xml call.

### `lib/entities.c` (73 lines)

XML entity quote/unquote. Holds a hard-coded mapping of the XML spec's predefined entities (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`) plus three Merry-attribute escape entries (`pipe`, `lbrace`, `rbrace`). Surface: `asciiToEntity` / `entityToAscii` mapping, plus `entifyString(string str, int ents...)` for the encoded form.

### `lib/xmd.c` (169 lines)

XMD tree construction and query helpers. Surface: `xmdElts`, `xmdText`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdRef`, `xmdRefRef`, `xmdRefAttributes`, `xmdWipeTags`, `xmdStripPcdata`, `xmdForceToData`, `attributesToMapping`, `xmdOptimize`.

### `lib/xmlgen.c` (214 lines)

ASCII XML generation. Walks an XMD tree and appends serialized output via the caller-supplied `append(string)` sink. Surface: `generate_xml(mixed data, object res, varargs string indent)`, `generate_pcdata`, plus the private helpers `xml_attr` and `xml_head`. `RIGHT_MARGIN = 70` controls line wrap.

### `lib/xmlparse.c` (674 lines)

ASCII XML parser. All-LPC lexer + recursive-descent parser. Returns an XMD tree or raises `LexErr` on invalid input. Surface: `parse_xml(mixed str, varargs string file, int peekflag, int looseflag)` is the entry point, wrapped publicly by `xml_daemon`'s `parse(string str)`; internally `Scan` / `ScanMerry` and the `p_virgin` / `p_oneof` / `p_ref` / `p_tag` / `p_attr` parse-state functions handle the lex/parse states. `convert(mixed content, varargs string ltype, int strip)` is a private helper that folds parsed content into its final XMD shape.

### `sys/xml_daemon.c` (187 lines)

The XML type daemon. Inherits `~/lib/xmlparse`, `~/lib/xmlgen`, `~/lib/xmd`. At `create()` time registers four types and three colours with `dtd_daemon`. Implements the DTD-callback API for the XML-typed values (`queryTypeColour`, `typedToAscii`, `asciiToTyped`, `testRawData`, etc.). `gen_xml(xmd)` is the public serialize entry, constructing a `/lib/StringBuffer` LWO via `new StringBuffer()` for the output sink (`clone_object` requires an `/obj/` path, which `/lib/StringBuffer` fails).

### `initd.c` (23 lines)

Boot trigger. Compiles the LWO data wrappers (`data/element`, `data/pcdata`, `data/samref`) before `sys/xml_daemon`, since `new_object()` does not auto-compile an uncompiled master and `xml_daemon`'s parse / gen paths depend on them. Boot order discipline: System's two-pass loop registers XML's resource owner in pass 1 so that Schema's cross-domain inherit of `/usr/XML/lib/entities` resolves at compile time, then loads XML's initd in pass 2 after Schema's initd has loaded `dtd_daemon`.

### `data/element.c`, `data/pcdata.c`, `data/samref.c` (30-32 lines each)

Lightweight-wrapper-object (LWO) data carriers. Each wraps parsed XML data of one kind and is structurally tagged via `object_name` so callers can dispatch on kind without reading the data field. The dispatchers `queryColour` and `queryColourValue` in `/lib/util/lpc` resolve `(object) -> (colour-int, data-array)`.

## Dependencies

XML depends on Schema being loaded first (`dtd_daemon` must exist when `xml_daemon::create()` registers types). System's domain-iteration order achieves this via alphabetical iteration plus the two-pass loop in `System/initd.c`.

XML's libraries (`xmd`, `xmlgen`, `xmlparse`, `entities`) are inheritable, so Vault, Marshal, and other downstream consumers pick up XML parse / generate capability by inheriting from `/usr/XML/lib/*`. Cross-domain access is granted via `set_global_access("XML", TRUE)` in `System/initd.c`.

## Where to next

- [schema.md](schema.md) — `dtd_daemon` is the registry XML registers with; `lib/dtd` is what XML inherits to query types
- [`src/usr/Marshal/`](../src/usr/Marshal/) — XmlBinding/stateimpex builds on top of this XML transport for state import/export
- [`src/lib/util/ascii.c`](../src/lib/util/ascii.c) and [`src/lib/util/lpc.c`](../src/lib/util/lpc.c) — the helper libs the XML transport uses
