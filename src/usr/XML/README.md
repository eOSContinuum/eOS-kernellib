# XML

The XML transport layer. XML parses ASCII XML to an internal XMD tree, generates ASCII XML from XMD, and registers the core XML types with the Schema subsystem's `dtd_daemon` so callers downstream of marshaling can resolve element / pcdata / samref / bool typed values through a single dispatcher.

XML is one of five subsystems in the cohesive Vault layout (Vault / Marshal / Schema / XML / Index). It is the format-implementation layer: where Schema defines what elements exist (format-neutral), XML defines how to wire-encode them. Future formats (CBOR, Gordian Envelope) would sit alongside as sibling format implementations, each registering its types with `dtd_daemon`.

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
├── initd.c                  compiles xml_daemon at boot
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
    └── samref.c             LWO wrapper for SAM-reference data (structural; no game-content interpretation)
```

## Boot sequence

System's initd loads XML's initd in alphabetical iteration after Schema (S) — Schema must precede XML so `dtd_daemon` exists when `xml_daemon::create()` calls `DTD->register_type(...)`. XML's initd:

1. `compile_object("sys/xml_daemon")` — compiles the daemon. At `create()` time it inherits `~/lib/xmlparse`, `~/lib/xmlgen`, `~/lib/xmd` (which transitively pull `lib/entities`), then calls `DTD->register_type(XML_ELEMENT | XML_PCDATA | XML_SAMREF | XML_BOOL)` and `DTD->register_colour(COL_ELEMENT | COL_PCDATA | COL_SAMREF)` against the now-loaded Schema's `dtd_daemon`.

After boot, Schema-mediated type dispatch resolves any XML-typed values through xml_daemon's handler methods (`query_type_colour`, `typed_to_ascii`, `ascii_to_typed`, `test_raw_data`, etc.).

## Internal vs wire format

XML uses two representations and a clear vocabulary for the boundary:

- **XML** is the ASCII serialization. The lexer in `xmlparse.c` reads it; the generator in `xmlgen.c` writes it. Public.
- **XMD** is the internal binary form. LPC mappings and arrays carrying parsed element data, anchored by LWO wrappers in `data/`. Internal.

`xmd.c` is named for the internal form; the helpers there (`xmdElts`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdOptimize`, etc.) operate on XMD trees, not on serialized XML strings.

## SUGAR and the SAM module: structural-only

SAM (SAM-A-Mole) is a SkotOS sugar-tag module for game content; it is excluded from this subsystem per the project's game-content exclusion. Three consequences in the XML lift:

- **The SUGAR daemon is absent.** `xmlgen.c::generate_pcdata` does not consult a sugar-tag daemon; it dispatches element / pcdata / samref directly.
- **The SAM-syntax brace form (`{ choice | choice }`) raises LexErr.** `xmlparse.c::p_oneof` no longer interprets it; the parser flags it as invalid input.
- **`samref.c` is preserved as a structural LWO.** The wrapper carries XML data with no semantic interpretation. xmlgen.c emits a literal `$(ref attrs)` form for any reader that wants it; no game-content rewrite happens in this layer. Removing samref.c would force a xmd.c / xmlparse.c rewrite, which is out of scope.

## SkotOS dependencies dropped

The XML lift is to a leaner platform layer. These SkotOS dependencies are not present:

- **SUGAR / SAM** — see above.
- **NREF / `<hard.h>` typed-literal forms** — `HARD()` / `HARDEN()` references in XML_BOOL handling are stripped; the lifted XML_BOOL is a simple `true` / `false` serialization.
- **`/lib/womble`** — a one-shot NRef-migration helper, never invoked from xmlparse body. Inherit dropped; `womble_xml()` callback dropped with it.
- **`/lib/string`** — replaced by `/lib/util/ascii` (`strip`, `strip_left`, `strip_right`, `lower_case`, plus `char_to_string` and `replace_strings` added at lift time).
- **`/lib/data` and `<fastarr.h>` / `<faststr.h>`** — `xml_daemon.c::gen_xml` uses a per-call `/lib/StringBuffer` clone instead of SkotOS's `/lib/data` accumulator + faststr macros. The `append_string` method becomes `append` at xmlgen.c's emission sites.
- **`/lib/array`, `/lib/mapping`** — replaced by `/lib/util/ascii` and `/lib/util/lpc` (which provides `member`, `reverseMapping`, `queryColour`, `queryColourValue`, `sysLog`, `dumpValue`).
- **Logging stubs (`DEBUG` / `Debug` / `XDebug`, `TLSD->query_tls_value`, `SYSLOGD->query_last_error`)** — mapped to no-ops. The `dump_value` references in their args are dead code. When a kernel-layer log facility lands, these wire to it.
- **SkotOS-only includes (`<SID.h>`, `<DTD.h>`, `<System.h>`, `<nref.h>`, `<SAM.h>`)** — dropped. The Schema-side `SID` and `DTD` constants are inline-defined per the LV-2 rename (`SID = /usr/Schema/sys/schema_daemon`, `DTD = /usr/Schema/sys/dtd_daemon`).
- **Merry data wrapper path** — references to `/usr/SkotOS/data/merry` rewritten to `/usr/Merry/data/merry`, anticipating a future Merry-subsystem lift.

## Naming convention

Function names within the XML transport were rewritten to camelCase at lift time, matching the `/lib/util/*` helper convention:

- `xmdElts`, `xmdText`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdRef`, `xmdRefRef`, `xmdRefAttributes`, `xmdWipeTags`, `xmdStripPcdata`, `xmdForceToData`, `xmdOptimize`, `attributesToMapping`
- `queryColour`, `queryColourValue`, `entityToAscii`, `asciiToEntity`
- `sysLog`, `dumpValue`

Two boundaries retain `snake_case` as a contract surface:

- The **DTD-callback API** in `xml_daemon.c` (`query_type_colour`, `typed_to_ascii`, `ascii_to_typed`, `test_raw_data`, `query_asciisize`, `query_asciiheight`, `query_checkboxed`, `default_value`, `query_state_root`). These are called by name through `dtd_daemon`'s dispatch, so all handlers registered with the daemon must agree on the names. A coordinated rename across handlers (xml_daemon + the future stateimpex) is a separate sweep, not a per-handler decision.
- `lower_case`, `strip`, `strip_left`, `strip_right` in `/lib/util/ascii` keep their pre-existing names (`lower_case` was already there before the lift; `strip*` were added alongside at lift time).

## File-by-file reference

### `src/include/XML.h` (14 lines)

Public type constants: `XML_ELEMENT`, `COL_ELEMENT`, `XML_SAMREF`, `COL_SAMREF`, `XML_PCDATA`, `COL_PCDATA`, `XML_MIXED`, `XML_BOOL`, plus the `XML` constant pointing at `/usr/XML/sys/xml_daemon`. Included by callers that need to reference XML types by name.

### `src/usr/XML/include/XMLIn.h` (302 lines)

Lexer state variables and macros for `lib/xmlparse.c`. Source-lifted verbatim from SkotOS. `DEBUG(...)` macro calls expand to no-ops in the lift's current logging story; the `dump_value` references inside those calls are dead code (never compiled). When a real kernel-layer log facility lands, `DEBUG` and `dump_value` wire here.

### `src/usr/XML/include/XMLOut.h` (88 lines)

Output-buffer macros for `lib/xmlgen.c`. Source-lifted verbatim. Operates against the StringBuffer-shape `append(string)` sink the lifted xml_daemon clones per gen_xml call.

### `lib/entities.c` (79 lines)

XML entity quote/unquote. Holds a hard-coded mapping of the XML spec's predefined entities (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`) plus three Merry-attribute escape entries (`pipe`, `lbrace`, `rbrace`). Surface: `asciiToEntity` / `entityToAscii` mapping, plus `entityString(string s)` for the encoded form.

### `lib/xmd.c` (176 lines)

XMD tree construction and query helpers. Surface: `xmdElts`, `xmdText`, `xmdElement`, `xmdAttributes`, `xmdContent`, `xmdRef`, `xmdRefRef`, `xmdRefAttributes`, `xmdWipeTags`, `xmdStripPcdata`, `xmdForceToData`, `attributesToMapping`, `xmdOptimize`.

### `lib/xmlgen.c` (227 lines)

ASCII XML generation. Walks an XMD tree and appends serialized output via the caller-supplied `append(string)` sink. Surface: `generate_xml(xmd, output, lookups...)`, `generate_xml_with_dtd(...)`, `generate_pcdata`, `generate_attribute`, `generate_element`. `RIGHT_MARGIN = 70` controls line wrap.

### `lib/xmlparse.c` (696 lines)

ASCII XML parser. All-LPC lexer + recursive-descent parser. Returns an XMD tree or raises `LexErr` on invalid input. Surface: `convert(string input)` is the entry point; internally `Scan` / `peek` / `ScanMerry` / `p_oneof` / `p_attributes` / `p_element` / `p_pcdata` handle the lex/parse states.

### `sys/xml_daemon.c` (196 lines)

The XML type daemon. Inherits `~/lib/xmlparse`, `~/lib/xmlgen`, `~/lib/xmd`. At `create()` time registers four types and three colours with `dtd_daemon`. Implements the DTD-callback API for the XML-typed values (`query_type_colour`, `typed_to_ascii`, `ascii_to_typed`, `test_raw_data`, etc.). `gen_xml(xmd)` is the public serialize entry, cloning a `/lib/StringBuffer` for the output sink.

### `initd.c` (15 lines)

Boot trigger. Compiles `sys/xml_daemon`. Boot order discipline: System's two-pass loop registers XML's resource owner in pass 1 so that Schema's cross-domain inherit of `/usr/XML/lib/entities` resolves at compile time, then loads XML's initd in pass 2 after Schema's initd has loaded `dtd_daemon`.

### `data/element.c`, `data/pcdata.c`, `data/samref.c` (~35 lines each)

Lightweight-wrapper-object (LWO) data carriers. Each wraps parsed XML data of one kind and is structurally tagged via `object_name` so callers can dispatch on kind without reading the data field. The dispatchers `queryColour` and `queryColourValue` in `/lib/util/lpc` resolve `(object) -> (colour-int, data-array)`.

## Dependencies

XML depends on Schema being loaded first (`dtd_daemon` must exist when `xml_daemon::create()` registers types). System's domain-iteration order achieves this via alphabetical iteration plus the two-pass loop in `System/initd.c`.

XML's libraries (`xmd`, `xmlgen`, `xmlparse`, `entities`) are inheritable, so Vault, Marshal, and other downstream consumers pick up XML parse / generate capability by inheriting from `/usr/XML/lib/*`. Cross-domain access is granted via `set_global_access("XML", TRUE)` in `System/initd.c`.

## See also

- `src/usr/Schema/` — `dtd_daemon` is the registry XML registers with; `lib/dtd` is what XML inherits to query types
- `src/usr/Marshal/` (lifts at LV-4.5c) — XmlBinding/stateimpex builds on top of this XML transport for SkotOS-style state import/export
- `src/lib/util/ascii.c` and `src/lib/util/lpc.c` — the helper libs the XML lift pulls from instead of SkotOS's `/lib/string`, `/lib/array`, `/lib/mapping`, `/lib/data`
