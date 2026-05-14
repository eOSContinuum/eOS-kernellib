<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Kernel Libraries

eOS-kernellib ships a small set of inheritable libraries under `src/lib/` for application authors to consume directly. Each library is an LPC class an application either inherits or instantiates via the canonical-name `#define` from a header in `src/include/`. This document is a topical reference; the source files at the cited paths are authoritative for the actual API surface.

For the LPC mechanics that make these libraries work (inherit syntax, type modifiers, lifecycle), see `doc/lpc-essentials.md`. For where these libraries fit in the substrate's tier model, see `doc/architecture.md`.

## Strings

| Class | File | Header | Role |
|---|---|---|---|
| `String` | `src/lib/String.c` | `<String.h>` | Immutable string wrapper with helper methods |
| `StringBuffer` | `src/lib/StringBuffer.c` | `<String.h>` | Efficient piecewise string accumulation; ring-buffer-backed chunk consolidation |

`StringBuffer` is the canonical choice for output construction where pieces arrive over time (HTTP response bodies, log message assembly, parser-generator output). Avoid string concatenation in tight loops; use `StringBuffer` instead.

## Persistent collections

| Class | File | Header | Role |
|---|---|---|---|
| `KVstore` | `src/lib/KVstore.c` | `<KVstore.h>` | Persistent key-value store with structural sharing |
| `KVnode` | `src/lib/KVnode.c` | `<KVstore.h>` | Internal tree-node implementation backing `KVstore` |

`KVstore` is the supported backing for application-level persistent collections beyond the host's built-in `mapping`. The structural-sharing layout means insertions and deletions share unchanged subtrees with prior versions, useful when an application keeps versioned snapshots of a collection in memory. The shipped `obj/kvnode.c` is the cloneable instantiation; the `KVNODE` macro in `<KVstore.h>` resolves to its path.

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
| `src/lib/util/ascii.c` | character math | ASCII case conversion, character classification |
| `src/lib/util/asn.c` | host kfuns `asn_*` | Big-integer (arbitrary-precision) arithmetic |
| `src/lib/util/base64.c` | character encoding | Base64 encode and decode |
| `src/lib/util/hex.c` | character encoding | Hexadecimal encode and decode |
| `src/lib/util/json.c` | `/sys/jsonencode`, `/sys/jsondecode` | JSON encode and decode |
| `src/lib/util/random.c` | host kfun `random` | Pseudo-random string generation |
| `src/lib/util/unicode.c` | UTF-8 encoding | Unicode code-point conversion |

These are inherited via `inherit "/lib/util/<name>"`. Because the wrapped functions are declared `static` (per LPC type-modifier semantics in `doc/lpc-essentials.md`), they are callable from the inheriting object and its children but not from arbitrary callers.

## Where to next

- **`doc/lpc-essentials.md`** -- LPC mechanics: inherit syntax, type modifiers, kfun calls, the patterns these libraries assume the reader knows.
- **`doc/architecture.md`** -- the substrate's tier model and where `src/lib/` fits in it.
- **`doc/application-authoring.md`** -- how an application's LPC source consumes these libraries from a tier-E domain.
