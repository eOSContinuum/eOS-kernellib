# Kernel libraries

eOS-kernellib ships a small set of inheritable libraries under `src/lib/` for application authors to consume directly. Each library is an LPC class an application either inherits or instantiates via the canonical-name `#define` from a header in `src/include/`. This document is a topical reference; the source files at the cited paths are authoritative for the actual API surface.

For the LPC mechanics that make these libraries work (inherit syntax, type modifiers, lifecycle), see `docs/lpc-essentials.md`. For where these libraries fit in the platform's tier model, see `docs/architecture.md`.

**Audience**: an LPC application author looking up which inheritable library serves a common need (strings, persistent collections, iteration, asynchronous control, time, utilities); assumes `docs/lpc-essentials.md` for inherit syntax and `docs/architecture.md` for the tier model that bounds where each library is callable from.

## Strings

| Class | File | Header | Role |
|---|---|---|---|
| `String` | `src/lib/String.c` | `<String.h>` | Immutable string wrapper with helper methods |
| `StringBuffer` | `src/lib/StringBuffer.c` | `<String.h>` | Efficient piecewise string accumulation; ring-buffer-backed chunk consolidation |

`StringBuffer` is the canonical choice for output construction where pieces arrive over time (HTTP response bodies, log message assembly, parser-generator output). Avoid string concatenation in tight loops; use `StringBuffer` instead.

## Persistent collections

| Class | File | Header | Role |
|---|---|---|---|
| `KVstore` | `src/lib/KVstore.c` | `<KVstore.h>` | Persistent key-value store backed by a B+ tree |
| `KVnode` | `src/lib/KVnode.c` | `<KVstore.h>` | Internal tree-node implementation backing `KVstore` |

`KVstore` is the supported backing for application-level persistent collections beyond the host's built-in `mapping`. It is implemented as a B+ tree whose nodes mutate in place on insert and delete; it holds a single current root and does not retain prior versions or share subtrees across versions. The shipped `obj/kvnode.c` is the cloneable instantiation; the `KVNODE` macro in `<KVstore.h>` resolves to its path.

## Property storage, identity, and inheritance

Three sibling libraries compose into the script-bearing-object pattern (`inherit "/lib/util/named"; inherit properties "/lib/util/properties"; inherit ur "/lib/util/ur";`) that Merry's invocation surface and the property-change dispatcher walk against. They are jointly load-bearing for any Merry-script-bearing host; `docs/merry-applications.md` shows the combined pattern.

### `/lib/util/properties.c`

An inheritable keyed property store on a host object. Inheriting hosts gain raw-key (case-preserving) and downcased-key access via `set_property` / `query_property` / `query_raw_property` / `query_prefixed_properties` / `set_raw_property`. The default `set_property` routes through the Merry daemon when loaded — `find_object("/usr/Merry/sys/merry")` returning non-nil triggers `MERRY->dispatch_set(this_object(), path, val)` for pre/main/post observer fan-out, cascade-depth bounding, and cycle detection; otherwise the call falls through to `set_downcased_property` directly (the storage-only path used during early bootstrap and by callers that need to bypass the dispatcher). The dispatcher integration is documented in `docs/dispatcher.md`. The property store is also where observer bindings live (`merry:on:<path>:<timing>` keys), which is why the dispatcher requires its registration targets to carry this library.

### `/lib/util/ur.c`

Ur-object infrastructure: parent and child tracking for the prototype-inheritance hierarchy. Inheriting makes an object both a possible ur-parent and ur-child; the surface is `set_ur_object(parent)` / `query_parent()` / `query_ur_children()`. Merry's `find_merry` / `find_merries` and the dispatcher's `find_observers` walk the ur chain via `query_parent()`, so a script or observer registered on an ancestor covers every descendant in the cohort. The `Ur:Hierarchy` schema element marshals the `parent` attribute through this surface.

### `/lib/util/named.c`

A mutable logical name distinct from the host's immutable `object_name()`. Inheriting objects call `set_object_name("Domain:path:name")` at create time; the registration wires through `~Index/sys/index_daemon`, so the name-to-object map is global and O(1)-invertible via `find_named(name)`. The Vault stores and respawns objects by these names, and the `OBJ(<name>)` literals in marshaled `lpc_obj` attributes resolve through the same registry.

## Iteration

| Class | File | Header | Role |
|---|---|---|---|
| `Iterable` | `src/lib/Iterable.c` | `<Iterator.h>` | Mixin for collections that produce iterators |
| `Iterator` | `src/lib/Iterator.c` | `<Iterator.h>` | Base iterator protocol |
| `IntIterator` | `src/lib/IntIterator.c` | `<Iterator.h>` | Iterator over integer ranges |

The iterator protocol decouples collection representation from traversal. An object that wants to be traversable inherits `Iterable` and returns an `Iterator` (or a subclass) from its iterator-producing method. Callers traverse without knowing the underlying representation.

## Asynchronous control

| Class | File | Header | Role |
|---|---|---|---|
| `Continuation` | `src/lib/Continuation.c` | `<Continuation.h>` | Composable async-control primitive over `call_out` |
| `IterativeContinuation` | `src/lib/IterativeContinuation.c` | `<Continuation.h>` | Iterates a continuation over a collection |
| `ChainedContinuation` | `src/lib/ChainedContinuation.c` | `<Continuation.h>` | Sequences continuations end-to-end |
| `DelayedContinuation` | `src/lib/DelayedContinuation.c` | `<Continuation.h>` | Defers a continuation by a configured delay |
| `DistContinuation` | `src/lib/DistContinuation.c` | `<Continuation.h>` | Distributes a continuation across multiple objects |

`call_out` (the primitive `kfun`) schedules a single deferred function call. The `Continuation` family composes that primitive into more structured async patterns. An application that needs work to fan out across multiple objects, sequence with cleanup steps, or iterate over a collection asynchronously uses these rather than open-coding `call_out` chains.

## Time

| Class | File | Header | Role |
|---|---|---|---|
| `Time` | `src/lib/Time.c` | `<Time.h>` | Local-time arithmetic and formatting |
| `GMTime` | `src/lib/GMTime.c` | `<Time.h>` | UTC-time variant of `Time` |

Both classes wrap the host's time kfuns (`time()`, `ctime()`) with a structured field interface (`year`, `month`, `day`, `hour`, etc.) and arithmetic methods. `<Time.h>` exports the canonical names plus constants like `SECSPERHOUR` and `DAYSPERWEEK` for time-arithmetic literals.

## Utilities

The `src/lib/util/` subdirectory holds small utility libraries that wrap kfun-level operations or singleton daemons in `static` functions an application inherits and calls:

| File | Wraps | Purpose |
|---|---|---|
| `src/lib/util/ascii.c` | character math | ASCII case conversion, character classification, strip helpers |
| `src/lib/util/delayed.c` | `call_out` | The `$delay()` continuation glue every Merry-script-bearing object inherits (`delayed_call` / `perform_delayed_call`) |
| `src/lib/util/file.c` | file kfuns | Path helpers (`paveWay`, `copyFile`, `writeDataToFile`) |
| `src/lib/util/fileparse.c` | `parse_string` | File-backed parser-runner; grammar loaded from a file (subclass of `parse.c`) |
| `src/lib/util/lpc.c` | misc | `dumpValue` stringification, `name()` logical-name fallback, `member`, XML-wrapper colour dispatch, `sysLog` / `info` / `debugLog` forwarders to `logd` |
| `src/lib/util/parse.c` | `parse_string` | Parser-runner over a yacc-shape grammar compiled to a scratch object |
| `src/lib/util/asn.c` | host kfuns `asn_*` | Big-integer (arbitrary-precision) arithmetic |
| `src/lib/util/base64.c` | character encoding | Base64 encode and decode |
| `src/lib/util/coercion.c` | LPC-literal grammar | Round-trip codec for simple LPC values (ints, floats, strings, object references, nil, arrays, mappings); the marshaling path behind `properties.c`'s `query_ascii_property` / `set_ascii_property` |
| `src/lib/util/hex.c` | character encoding | Hexadecimal encode and decode |
| `src/lib/util/json.c` | `/sys/jsonencode`, `/sys/jsondecode` | JSON encode and decode |
| `src/lib/util/random.c` | host kfun `random` | Pseudo-random string generation |
| `src/lib/util/unicode.c` | UTF-8 encoding | Unicode code-point conversion |
| `src/lib/util/url.c` | character encoding | URL encode and decode (the Vault's on-disk name segments) |

These are inherited via `inherit "/lib/util/<name>"`. Because the wrapped functions are declared `static` (per LPC type-modifier semantics in `docs/lpc-essentials.md`), they are callable from the inheriting object and its children but not from arbitrary callers.

## Where to next

- **`docs/lpc-essentials.md`** — LPC mechanics: inherit syntax, type modifiers, kfun calls, the patterns these libraries assume the reader knows.
- **`docs/architecture.md`** — the platform's tier model and where `src/lib/` fits in it.
- **`docs/application-authoring.md`** — how an application's LPC source consumes these libraries from a tier-E domain.
