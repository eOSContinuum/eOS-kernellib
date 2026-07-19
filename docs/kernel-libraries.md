# Kernel libraries

eOS-kernellib ships a small set of inheritable libraries under `src/lib/` for application authors to consume directly. Each library is an LPC class an application either inherits or instantiates via the canonical-name `#define` from a header in `src/include/`. This document is a topical reference; the source files at the cited paths are authoritative for the actual API surface.

For the LPC mechanics that make these libraries work (inherit syntax, type modifiers, lifecycle), see `docs/lpc-essentials.md`. For where these libraries fit in the platform's tier model, see `docs/architecture.md`.

**Audience**: an LPC application author looking up which inheritable library serves a common need (strings, persistent collections, large arrays, iteration, asynchronous control, time, utilities); assumes `docs/lpc-essentials.md` for inherit syntax and `docs/architecture.md` for the tier model that bounds where each library is callable from.

Each library below carries a per-class block listing its application-facing signatures. Each bullet gives a function's parameter and return types (the `varargs`, `mixed *`, and `args...` forms are LPC syntax; see `docs/lpc-essentials.md`); the `atomic` modifier is shown where a function runs as an atomic transaction, while the visibility modifiers (`static`, `nomask`) are omitted for brevity (the `src/lib/util/` helpers are `static` except where noted). `create` is the constructor the driver calls on instantiation, so its parameters are the arguments passed at `new_object` / `clone_object` time. Purely internal functions (declared `private`, or infrastructure callbacks) are omitted; where a class carries a large internal surface, the omission is noted.

## Strings

| Class | File | Header | Role |
|---|---|---|---|
| `String` | `src/lib/String.c` | `<String.h>` | Immutable string wrapper with helper methods |
| `StringBuffer` | `src/lib/StringBuffer.c` | `<String.h>` | Efficient piecewise string accumulation; ring-buffer-backed chunk consolidation |

`StringBuffer` is the canonical choice for output construction where pieces arrive over time (HTTP response bodies, log message assembly, parser-generator output). Avoid string concatenation in tight loops; use `StringBuffer` instead.

### `String`

Inherits `Iterable` (a `String` iterates over its characters); private-inherits `/lib/util/unicode` for case folding.

- `create(mixed data, varargs string utf8)` -- constructor: wrap `data`; pass the literal `"UTF8"` as the second argument to decode `data` as a UTF-8 byte string (any other non-nil value errors)
- `int length()` -- character count (byte count when this wraps bytes)
- `int isBytes()` -- true if the content is byte-range only (no character above 0xff)
- `StringBuffer buffer(varargs string utf8)` -- a `StringBuffer` over the whole content
- `StringBuffer bufferRange(varargs mixed from, mixed to)` -- a `StringBuffer` over a sub-range
- `string utf8()` -- the content as a UTF-8 byte string
- `int compare(mixed str)` / `int equals(mixed str)` -- ordered comparison / equality against a `string` or `String`
- `int compareIgnoreCase(mixed str)` / `int equalsIgnoreCase(mixed str)` -- case-folded comparison / equality
- `String capitalize()` / `String toUpperCase()` / `String toLowerCase()` -- case transforms, each returning a new `String`
- `mixed iteratorStart(mixed from, mixed to)` / `mixed *iteratorNext(mixed state)` / `int iteratorEnd(mixed state)` -- the iterator protocol supplied to `Iterable`
- operators: `str[i]` (character code at `i`), `str[a..b]` (a `String` subrange), `str + other` (concatenation), and `<` / `<=` / `>` / `>=` (comparison); `str[i] = c` errors, as a `String` is immutable

### `StringBuffer`

- `create(varargs mixed str, int max)` -- constructor: optional initial content; `max` is the accumulated-length threshold at which buffered pieces are consolidated (defaults to the host string-size limit)
- `void append(mixed str)` -- append a `string` or character-array (`int *`) chunk
- `mixed chunk()` -- consolidate the buffered pieces and return the accumulated content
- `int length()` -- total accumulated length

## Persistent collections

| Class | File | Header | Role |
|---|---|---|---|
| `BTree` | `src/lib/BTree.c` | `<BTree.h>` | Ordered key-value B+ tree over light-weight nodes; the base `KVstore` extends |
| `BTnode` | `src/lib/BTnode.c` | `<BTree.h>` | Internal tree-node implementation backing `BTree` |
| `KVstore` | `src/lib/KVstore.c` | `<KVstore.h>` | Persistent key-value store: a `BTree` whose nodes are persistent clones |
| `KVnode` | `src/lib/KVnode.c` | `<KVstore.h>` | Cloneable tree-node subclass backing `KVstore` |

**Choosing a collection.** A plain `mapping` is right until one of two bounds bites: the driver caps a single mapping at 32,767 key-value pairs (the `array_size` knob governs mappings too, and it is already at its driver ceiling -- `docs/operations.md` Sizing a workload), or the holding object's dataspace grows to where its unit residency and per-snapshot share matter. Past that: `/lib/Array` for integer-indexed sequences (32767-element chunks presented as one value, up to 32767^2 elements, still one dataspace); `KVstore` when keys are strings and the set should page at node granularity instead of as one dataspace, at the price of `objects`-table slots for its node clones (roughly N/(fan-out/2) of them); `BTree` directly when ordered traversal over any comparable key type matters and light-weight nodes inside one dataspace are acceptable. A wrong early choice is a live-state migration later (`docs/common-tasks.md` Migrate live state after a data-shape change), so size against `docs/operations.md` Sizing a workload before the first real domain model.

`KVstore` is the supported backing for application-level persistent collections beyond the host's built-in `mapping`. The B+ tree mechanics live in `BTree`, which `KVstore` extends: `BTree` backs its nodes with light-weight `BTnode` objects and accepts any key type the host's comparison operators order, while `KVstore` narrows keys to strings and overrides node creation to clone the shipped `/obj/kvnode` (the `KVNODE` macro in `<KVstore.h>` resolves to its path), so each node is a persistent clone destructed when its tree deletes it. Both mutate nodes in place on insert and delete, hold a single current root, and do not retain prior versions or share subtrees across versions.

### `BTree`

Inherits `Iterable` (and private-inherits `/lib/util/random` for access-key generation). Every mutator is `atomic` (all-or-nothing under the driver's transaction model). Keys may be any type the host's comparison operators order, including keys the language treats as false (integer `0`, floating-point `0.0`): the node implementation checks key presence against nil, not truthiness. Iteration yields `({ key, value })` pairs in key order.

- `atomic create(int maxSize, varargs string accessKey, string nodePath)` -- constructor: node fan-out `maxSize`, optional access key (a random 32-char key if omitted), optional node-object path (light-weight `BTnode` objects by default)
- `mixed get(mixed key)` -- the value stored under `key`, or nil
- `atomic int set(mixed key, mixed value)` -- insert or replace; a nil `value` deletes the key; returns the element-count delta (1 on insert, -1 on delete, 0 on replace or no-op)
- `atomic int add(mixed key, mixed value)` -- insert; errors if `key` already exists
- `atomic int change(mixed key, mixed value)` -- replace; errors if `key` does not exist
- `atomic void remove()` -- remove the entire tree, deleting all nodes
- `mixed iteratorStart(mixed from, mixed to)` / `mixed *iteratorNext(mixed state)` / `int iteratorEnd(mixed state)` -- the iterator protocol supplied to `Iterable`; a bounded iterator walks `from` .. `to` in key order, and walks backwards when `from > to`
- operators: `tree[key]` is `get`, `tree[key] = value` is `set`

### `BTnode`

Internal B+ tree node backing `BTree`; applications use `BTree`, not `BTnode` directly. Nodes are light-weight objects; `KVnode` subclasses this into a persistent clone for `KVstore`.

### `KVstore`

Inherits `BTree`: the mutators wrap `BTree`'s atomic implementations with key validation (a nil key errors), keys are strings, and the nodes are persistent clones.

- `atomic create(int maxSize, varargs string accessKey, string nodePath)` -- constructor: node fan-out `maxSize`, optional access key (a random 32-char key if omitted), optional node-object path (`KVNODE` by default)
- `mixed get(string key)` -- the value stored under `key`, or nil
- `int set(string key, mixed value)` -- insert or replace; a nil `value` deletes the key
- `int add(string key, mixed value)` -- insert; errors if `key` already exists
- `int change(string key, mixed value)` -- replace; errors if `key` does not exist
- inherited from `BTree`: `remove()`, the iterator protocol (bounded and reverse iteration), and the `store[key]` / `store[key] = value` operators

### `KVnode`

Internal tree node backing `KVstore`: inherits `BTnode`, creating nodes as clones and destructing them on removal. Applications use `KVstore`, not `KVnode` directly. The `KVNODE` macro (`<KVstore.h>`) resolves to the cloneable `/obj/kvnode`.

## Large arrays

| Class | File | Header | Role |
|---|---|---|---|
| `Array` | `src/lib/Array.c` | `<Array.h>` | Chunked array presenting up to 32767^2 elements as one indexable value |

The host caps a single LPC array at its configured array-size limit. `Array` stores elements in chunks of 32767 and presents them as one zero-indexed value of up to 32767 * 32767 (1,073,676,289) elements, with the familiar array operators reproduced over the chunked representation. Indexed assignment mutates in place; every other operator returns a new `Array`. An `Array` is instantiated as a light-weight object (`new Array(...)`), an embedded value in its creator's dataspace rather than an independently managed clone.

### `Array`

Inherits `Iterable`.

- `create(mixed arg)` -- constructor: an int allocates that many nil elements (0 .. 32767^2, larger errors); an LPC array copies its contents; the `Array`-argument form is internal plumbing for the operator implementations
- `int size()` -- the element count
- `mixed iteratorStart(mixed from, mixed to)` / `mixed *iteratorNext(mixed state)` / `int iteratorEnd(mixed state)` -- the iterator protocol supplied to `Iterable`, over optional integer index bounds (invalid bounds error)
- operators: `arr[i]` / `arr[i] = v` (range-checked), `arr[a..b]` (a new `Array` subrange; invalid bounds error), `arr + other` (concatenation), `arr - other` (remove `other`'s elements), `arr & other` (intersection), `arr | other` (set-union: `arr` plus `other`'s elements not already present), `arr ^ other` (symmetric difference) -- the chunked equivalents of the host's array operators

## Property storage, identity, and inheritance

Three sibling libraries compose into the script-bearing-object pattern (`inherit "/lib/util/named"; inherit properties "/lib/util/properties"; inherit ur "/lib/util/ur";`) that Merry's invocation surface and the property-change dispatcher walk against. They are jointly load-bearing for any Merry-script-bearing host; `docs/merry-applications.md` shows the combined pattern.

### `/lib/util/properties.c`

An inheritable keyed property store on a host object. Inheriting hosts gain raw-key (case-preserving) and downcased-key access via `set_property` / `query_property` / `query_raw_property` / `query_prefixed_properties` / `set_raw_property`. The default `set_property` routes through the Merry daemon when loaded -- `find_object("/usr/Merry/sys/merry")` returning non-nil triggers `MERRY->dispatch_set(this_object(), prop, val, caller)` (the captured writer program threaded through as `caller`) for pre/main/post observer fan-out, cascade-depth bounding, and cycle detection; otherwise the call falls through to `set_downcased_property` directly (the storage-only path used during early bootstrap and by callers that need to bypass the dispatcher). The dispatcher integration is documented in `docs/dispatcher.md`. The property store is also where observer bindings live (`merry:on:<path>:<timing>` keys), which is why the dispatcher requires its registration targets to carry this library.

- `mixed set_property(string prop, mixed val, varargs mixed extra...)` -- public write; routes through the Merry dispatcher when the daemon is loaded, else writes directly
- `mixed query_property(string prop)` -- read a property (lower-cases the key first)
- `void set_raw_property(string prop, mixed val)` -- write directly, bypassing dispatch (the dispatcher's own write step; also for performance-sensitive callers)
- `mixed query_raw_property(string prop)` -- read by the exact key, without the lower-casing pass
- `mixed query_downcased_property(string prop)` -- read by an already-lower-cased key
- `mapping query_prefixed_properties(string prefix)` -- every stored key sharing `prefix` (the Merry-script lookup surface)
- `void clear_property(string prop)` -- remove a single property
- `void clear_all_properties()` -- reset the table, preserving the `merry:*` runtime wiring (observer registrations, dispatcher state)
- `string query_ascii_property(string prop)` / `void set_ascii_property(string prop, string ascii)` -- marshal a value through the `/lib/util/coercion` codec (the `Core:Entry` schema's property surface)
- `mapping query_properties(varargs int opaque)` -- the property map; excludes the reserved `merry:*` namespace unless a non-zero argument is passed
- `string *query_property_indices(varargs int opaque)` -- the property keys (same `opaque` rule)
- `void add_properties(mapping map)` -- merge a map of properties into the table
- `mapping query_intrinsic_properties()` -- the full raw table, including `merry:*`
- `string queryStateRoot()` -- the schema state-root name for marshaling (default `"Core:Entries"`; inheritors override, e.g. the vault-app `thing.c` binds `"MyApp:Thing"`)

### `/lib/util/ur.c`

Ur-object infrastructure: parent and child tracking for the prototype-inheritance hierarchy. Inheriting makes an object both a possible ur-parent and ur-child; the surface is `set_ur_object(parent)` / `query_parent()` / `query_ur_children()`. Merry's `find_merry` / `find_merries` and the dispatcher's `find_observers` walk the ur chain via `query_parent()`, so a script or observer registered on an ancestor covers every descendant in the cohort. The `Ur:Hierarchy` schema element marshals the `parent` attribute through this surface.

- `atomic void set_ur_object(object ob)` -- set (or clear) the ur-parent; errors if the assignment would form a cycle
- `object query_parent()` -- the ur-parent object, or nil
- `object *query_ur_children()` -- every direct ur-child
- `int query_child_count()` -- the number of direct ur-children
- `int is_child_of(object ancestor)` -- true if `ancestor` lies on this object's ur-chain
- `object query_first_child()` -- the first ur-child, or nil
- `object query_next_ur_sibling()` / `object query_previous_ur_sibling()` -- navigate siblings within the parent's child cohort

The child-linkage mutators (`add_child` / `delete_child` / `update_child` / `adopt_ur_child` / `release_ur_child` / `patch_*`) are internal machinery driven by `set_ur_object` (through the ur-parent) and object upgrades, not called directly by applications.

### `/lib/util/named.c`

A mutable logical name distinct from the host's immutable `object_name()`. Inheriting objects call `set_object_name("Domain:path:name")` at create time; the registration wires through `~Index/sys/index_daemon`, so the name-to-object map is global and O(1)-invertible via `find_named(name)`. The Vault stores and respawns objects by these names, and the `OBJ(<name>)` literals in marshaled `lpc_obj` attributes resolve through the same registry.

- `void set_object_name(string lname)` -- assign the mutable logical name (registers with the index daemon)
- `string query_object_name()` -- the current logical name
- `object find_named(string lname)` -- resolve a logical name to its object, globally and in O(1)

## Iteration

| Class | File | Header | Role |
|---|---|---|---|
| `Iterable` | `src/lib/Iterable.c` | `<Iterator.h>` | Mixin for collections that produce iterators |
| `Iterator` | `src/lib/Iterator.c` | `<Iterator.h>` | Base iterator protocol |
| `IntIterator` | `src/lib/IntIterator.c` | `<Iterator.h>` | Iterator over integer ranges |

The iterator protocol decouples collection representation from traversal. An object that wants to be traversable inherits `Iterable` and returns an `Iterator` (or a subclass) from its iterator-producing method. Callers traverse without knowing the underlying representation.

### `Iterable`

The mixin a traversable collection inherits. The collection supplies the three-function protocol; `Iterable` turns it into `Iterator` objects.

- `Iterator iterator(varargs mixed from, mixed to)` -- produce an `Iterator` over this collection, optionally bounded by `from` / `to`
- `mixed iteratorStart(mixed from, mixed to)` / `mixed *iteratorNext(mixed state)` / `int iteratorEnd(mixed state)` -- the protocol the inheriting collection must define (forward-declared here)

### `Iterator`

- `create(Iterable obj, mixed from, mixed to)` -- constructor: bind to a collection and an optional range
- `void reset()` -- restart traversal from the beginning
- `mixed current()` -- the current element
- `mixed next()` -- advance and return the next element
- `int end()` -- true once traversal is exhausted
- `Iterable iterated()` -- the collection being traversed

### `IntIterator`

Inherits `Iterable` and `Iterator`; walks an integer range.

- `create(int from, int to)` -- constructor: iterate the range `from` .. `to`
- `mixed iteratorStart(mixed from, mixed to)` / `mixed *iteratorNext(mixed state)` / `int iteratorEnd(mixed state)` -- the range-walking protocol

## Asynchronous control

| Class | File | Header | Role |
|---|---|---|---|
| `Continuation` | `src/lib/Continuation.c` | `<Continuation.h>` | Composable async-control primitive over `call_out` |
| `IterativeContinuation` | `src/lib/IterativeContinuation.c` | `<Continuation.h>` | Iterates a continuation over a collection |
| `ChainedContinuation` | `src/lib/ChainedContinuation.c` | `<Continuation.h>` | Sequences continuations end-to-end |
| `DelayedContinuation` | `src/lib/DelayedContinuation.c` | `<Continuation.h>` | Defers a continuation by a configured delay |
| `DistContinuation` | `src/lib/DistContinuation.c` | `<Continuation.h>` | Distributes a continuation across multiple objects |

`call_out` (the primitive `kfun`) schedules a single deferred function call. The `Continuation` family composes that primitive into more structured async patterns. An application that needs work to fan out across multiple objects, sequence with cleanup steps, or iterate over a collection asynchronously uses these rather than open-coding `call_out` chains.

### `Continuation`

Build a chain of steps with `add` / `chain` (or the `+` / `>>` operators), then start it once with one of the `run*` modes. Each `run*` is `atomic` and one-shot (a second start errors).

- `create(varargs string func, mixed args...)` -- constructor: an initial function name and its arguments
- `object add(mixed func, mixed args...)` -- append a function (or merge another `Continuation`); returns `this_object()` for fluent chaining
- `object chain(mixed func, mixed args...)` -- append a function whose call receives the previous step's return value; returns `this_object()`
- `atomic void runNext(mixed args...)` -- start the chain, to run at the next opportunity
- `atomic void runParallel(mixed args...)` -- start the chain's steps concurrently
- `atomic void runNew(mixed args...)` -- start the chain independently (no leading delay permitted)
- `int started()` -- true once a `run*` mode has fired
- operators: `cont + func` is `add`, `cont >> func` is `chain` (each operates on a copy)

### `ChainedContinuation`

Inherits `Continuation`.

- `create(string func, mixed args...)` -- sequence `func` end-to-end onto the continuation

### `DelayedContinuation`

Inherits `Continuation`.

- `create(mixed delay)` -- defer the continuation by `delay`

### `IterativeContinuation`

Inherits `Continuation`.

- `create(string func, Iterator iter, mixed args...)` -- run `func` once for each element the `Iterator` yields

### `DistContinuation`

Inherits `Continuation`.

- `create(object *objs, mixed timeout, string func, mixed args...)` -- distribute `func` across `objs`, bounded by `timeout`

## Time

| Class | File | Header | Role |
|---|---|---|---|
| `Time` | `src/lib/Time.c` | `<Time.h>` | Epoch-seconds value with comparison and GMT `ctime` formatting |
| `GMTime` | `src/lib/GMTime.c` | `<Time.h>` | A `Time` constructed from a GMT `ctime`-style string |

Both wrap an epoch-seconds value with equality and ordered comparison; `Time`'s `gmctime()` formats it as a GMT `ctime`-style string and `GMTime` parses that form back into a `Time`. `<Time.h>` exports the canonical names plus constants like `SECSPERHOUR` and `DAYSPERWEEK` for time-arithmetic literals in caller code.

### `Time`

- `create(int time, varargs float mtime)` -- constructor: whole seconds since the epoch, optional fractional seconds
- `int time()` -- the whole-seconds value
- `float mtime()` -- the fractional-seconds value
- `int equals(Time t)` -- equality against another `Time`
- `string gmctime()` -- a `ctime`-style formatted string (`"Wdy Mon DD HH:MM:SS YYYY"`)
- operators: `<`, `<=`, `>`, `>=` compare two `Time` values

### `GMTime`

Inherits `Time`; constructs from a formatted GMT string (the inverse of `Time`'s `gmctime()`).

- `create(string gmtime)` -- parse a `ctime`-style GMT string into epoch seconds; all `Time` accessors and comparisons are inherited

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
| `src/lib/util/cbor.c` | binary parsing | CBOR (RFC 8949) decoder for the deterministic subset WebAuthn/CTAP2 payloads use |
| `src/lib/util/coercion.c` | LPC-literal grammar | Round-trip codec for simple LPC values (ints, floats, strings, object references, nil, arrays, mappings); the marshaling path behind `properties.c`'s `query_ascii_property` / `set_ascii_property` |
| `src/lib/util/cose.c` | key extraction | COSE_Key (RFC 9052/9053) public-key extraction to the crypto kfuns' verify shapes (ES256, Ed25519) |
| `src/lib/util/hex.c` | character encoding | Hexadecimal encode and decode |
| `src/lib/util/json.c` | `/sys/jsonencode`, `/sys/jsondecode` | JSON encode and decode |
| `src/lib/util/random.c` | host kfun `random` | Pseudo-random string generation |
| `src/lib/util/unicode.c` | UTF-8 encoding | Unicode code-point conversion |
| `src/lib/util/url.c` | character encoding | URL encode and decode (the Vault's on-disk name segments) |
| `src/lib/util/webauthn.c` | crypto kfuns | WebAuthn ceremony verification (registration, assertion) as pure functions over caller-supplied payloads |

These are inherited via `inherit "/lib/util/<name>"`. Most of the wrapped functions are declared `static` (per LPC type-modifier semantics in `docs/lpc-essentials.md`), so they are callable from the inheriting object and its children but not from arbitrary callers; a few entry points are public (notably `delayed_call` and the `parse.c` / `fileparse.c` runner queries).

### `src/lib/util/ascii.c`

- `string capitalize(string str)` / `string lower_case(string str)` / `string upper_case(string str)` -- ASCII case transforms
- `string stringify(string str)` -- escape backslash, double-quote, and newline in a string
- `string strip_left(string str, varargs int leave_newlines)` / `string strip_right(string str, varargs int leave_newlines)` / `string strip(string str, varargs int leave_newlines)` -- trim leading / trailing / both-side whitespace
- `string float2string(float flt)` -- format a float
- `string char_to_string(int c)` -- a one-character string from a character code
- `string replace_strings(string str, string swaps...)` -- apply a set of substring substitutions

### `src/lib/util/asn.c`

Arbitrary-precision integers are carried as byte strings; these convert and measure that representation for the host's `asn_*` kfuns.

- `string encode(int number)` -- the byte-string representation of `number`
- `int decode(string str)` -- the integer value of a byte-string representation
- `string extend(string str, int length)` / `string unsignedExtend(string str, int length)` -- sign- / zero-extend to `length` bytes
- `int bits(string str)` -- the bit length
- `string reverse(string str)` -- reverse the byte order

### `src/lib/util/base64.c`

- `string encode(string str)` / `string decode(string str)` -- standard Base64
- `string urlEncode(string str)` / `string urlDecode(string str)` -- URL-safe Base64 alphabet

### `src/lib/util/cbor.c`

Strict decoder for the deterministic CBOR subset WebAuthn and CTAP2 payloads use: integers, byte and text strings, arrays, maps (integer or string keys), and false/true/null (mapped to `0`/`1`/`nil`). Indefinite lengths, tags, floats, undefined, duplicate map keys, truncation, and integers that do not fit an LPC int all raise errors rather than returning partial values; a null map value is rejected because assigning `nil` deletes a mapping entry.

- `mixed decode(string data)` -- decode a string holding exactly one CBOR data item (trailing bytes are an error)
- `mixed *decodePrefix(string data, int offset)` -- decode the item starting at `offset`; returns `({ value, next })` with `next` the offset past the item, for values embedded mid-stream (a COSE key inside authenticator data)

### `src/lib/util/coercion.c`

- `string encodeValue(mixed value)` -- encode a simple LPC value (int, float, string, object reference, nil, array, mapping) to its literal form
- `mixed decodeValue(string str)` -- decode such a literal back to the LPC value

### `src/lib/util/cose.c`

COSE_Key (RFC 9052/9053) public-key extraction for the credential algorithms the platform verifies natively. Unsupported key types, curve or algorithm mismatches, and malformed coordinates raise errors.

- `string *verifyKey(mapping coseKey)` -- map a decoded COSE_Key to `({ scheme, key })`: the crypto-kfun scheme prefix (`"ECDSA-SECP256R1-SHA256"` or `"Ed25519"`) and the raw public key in the shape `decrypt("<scheme> verify", key, signature, message)` expects (the uncompressed EC point `04 || X || Y` for P-256; the raw 32-byte key for Ed25519)

### `src/lib/util/delayed.c`

- `void delayed_call(object ob, string fun, mixed delay, mixed args...)` -- schedule `ob->fun(args...)` after `delay` (the `$delay()` glue)
- `void perform_delayed_call(object ob, string fun, mixed *args)` -- the call-out target that performs the deferred call

### `src/lib/util/file.c`

- `void paveWay(string path)` -- create any missing parent directories for `path`
- `void copyFile(string src, string dst)` -- copy a file
- `void writeDataToFile(string file, object data)` -- write an object's data to a file

### `src/lib/util/fileparse.c`

Inherits `parse.c`; loads the grammar from a file.

- `create(string scratch, string file, varargs int debug)` -- constructor: scratch-object path, grammar file, optional debug flag
- `mixed *parse_string(string str, varargs int trees)` -- parse `str` against the file's grammar
- `void reset_fileparse()` -- reload the grammar file

### `src/lib/util/hex.c`

- `string encode(int number, varargs int digits)` / `string encodeUpper(int number, varargs int digits)` -- an integer as lower- / upper-case hex, optionally zero-padded to `digits`
- `string format(string bytes)` / `string formatUpper(string bytes)` -- a byte string as lower- / upper-case hex
- `int decode(string str)` -- the integer value of a hex string
- `string decodeString(string str)` -- the byte string of a hex string

### `src/lib/util/json.c`

- `string encode(mixed value)` -- encode a value to JSON
- `mixed decode(string str)` -- decode JSON to an LPC value

### `src/lib/util/lpc.c`

- `string dumpValue(mixed value)` -- a readable string rendering of any LPC value (for debug output)
- `int member(mixed item, mixed *arr)` -- true if `item` is in `arr`
- `string name(object ob)` -- the object's logical name, falling back to `object_name()`
- `object findOrLoad(string path)` -- find the object at `path`, compiling it if it is not loaded
- `mixed queryColourValue(mixed value)` -- unwrap an XML `element` / `pcdata` / `samref` wrapper to its underlying data (non-wrapper values pass through unchanged)
- `int queryColour(mixed value)` -- the colour constant (`COL_ELEMENT` / `COL_PCDATA` / `COL_SAMREF`) classifying an XML wrapper value
- `mapping reverseMapping(mapping m)` -- a mapping with keys and values swapped
- `void info(string msg)` / `void sysLog(string msg)` / `void debugLog(string msg)` -- forward a message to `logd`

### `src/lib/util/parse.c`

- `create(string scratch, varargs int debug)` -- constructor: scratch-object path, optional debug flag
- `mixed *parse_string(string grammar, string str, varargs int trees)` -- parse `str` against `grammar`
- `string query_transformed_grammar()` -- the compiled grammar text
- `object query_parse_object()` -- the scratch parse object
- `void destroy_parse_object()` -- discard the scratch parse object

### `src/lib/util/random.c`

- `string random_string(int length)` -- a pseudo-random string of `length` bytes

### `src/lib/util/unicode.c`

Operate on a single Unicode code point.

- `int isLowerCase(int c)` / `int isUpperCase(int c)` / `int isTitleCase(int c)` -- case classification
- `int toLowerCase(int c)` / `int toUpperCase(int c)` / `int toTitleCase(int c)` -- case conversion
- `int foldCase(int c)` -- the case-folded code point (for case-insensitive comparison)

### `src/lib/util/url.c`

- `string urlEncode(string str)` -- percent-encode reserved characters
- `string urlDecode(string str)` -- decode a percent-encoded string

### `src/lib/util/webauthn.c`

WebAuthn (W3C Web Authentication Level 2) ceremony verification as pure functions: no challenge state, no credential store, no side effects. The caller supplies the relying-party id, origin, and the challenge it issued; each function returns the parsed result or raises a `webauthn: ...` error naming the first failed check. Scope matches the platform's TOFU posture: attestation format `"none"` only, ES256 and Ed25519 credentials. Needs the host crypto module (SHA-256, the verify kfuns). The System ceremony daemon (`docs/system-daemons.md` webauthnd) composes these with the identity registry; `examples/webauthn-app/` exercises them directly against foreign-generated vectors.

- `mapping verifyRegistration(string rpId, string origin, string challenge, string clientDataJSON, string attestationObject)` -- verify a registration (client-data type/challenge/origin, `fmt "none"` with an empty attStmt, rpIdHash, user-present and attested-credential flags) and return the credential as identityd row keys plus `"credentialId"` (the raw id)
- `int verifyAssertion(string rpId, string origin, string challenge, string scheme, string key, string clientDataJSON, string authenticatorData, string signature)` -- verify an assertion (client data, rpIdHash, user-present, then the signature over `authenticatorData || SHA-256(clientDataJSON)`) and return its signature counter; counter replay policy is the caller's

## Where to next

- **[`docs/lpc-essentials.md`](lpc-essentials.md)** -- LPC mechanics: inherit syntax, type modifiers, kfun calls, the patterns these libraries assume the reader knows.
- **[`docs/architecture.md`](architecture.md)** -- the platform's tier model and where [`src/lib/`](../src/lib/) fits in it.
- **[`docs/application-authoring.md`](application-authoring.md)** -- how an application's LPC source consumes these libraries from a tier-E domain.
