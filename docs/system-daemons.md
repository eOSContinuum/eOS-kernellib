# System-daemon application surface

The signature reference for the daemons an author is told to consume elsewhere in this doc set: the compile-graph recorder (objectd), the upgrade coordinator (upgraded), the error manager (errord), the logger (logd), the capability store (capabilityd), and the logical-name registry (the Index daemon). Format follows `docs/dispatcher.md`'s Application surface: per-function signature, gating, semantics. The source files are authoritative.

**Audience**: a System-tier or application author about to call one of these daemons directly and needing the exact contract, after the owning concept doc (`docs/code-lifecycle.md`, `docs/operations.md`, `docs/capability.md`, `docs/schema.md`) has explained when to.

Gating vocabulary: SYSTEM() admits callers under `/usr/System`; KERNEL() admits kernel-tier callers; a `previous_program()` gate admits exactly the named program. What a gated function does for an outside caller varies by function -- silent nil, an empty return, or an error -- and is noted where it matters; do not probe a gate by calling it.

## objectd -- `src/usr/System/sys/objectd.c`

The compile-time program graph: which programs exist, what each includes and inherits. The driver-hook event surface (compile, destruct, touch, and the rest) is covered in `docs/code-lifecycle.md`; the query surface below is SYSTEM()-gated.

### `string query_path(int index)`

The object path registered at program index `index`, or nil.

### `int *query_issues(string path)`

The active program issue indices for `path`. A library recompiled while inherited can carry more than one live issue; a path with none returns an empty array.

### `string *query_includes(int index)`

The files the program at `index` includes.

### `int **query_included(string path)`

The indices of programs that include `path`. The outer array's grouping is a storage artifact (buckets bounded by the host's array-size limit), not a per-program structure; flatten before use.

### `int *query_inherits(int index)`

The indices of programs the program at `index` inherits.

### `int **query_inherited(int index)`

The indices of programs that inherit the program at `index`, with the same bucket-shaped outer array as `query_included`. This is the record the upgrade daemon walks when a library recompile must cascade. The array-returning queries in this group return an empty array both for a caller outside the SYSTEM() gate and for a path or index with no entries.

## upgraded -- `src/usr/System/sys/upgraded.c`

The live-upgrade coordinator behind the console's `upgrade` verb (`docs/changing-a-running-system.md` describes the ladder; `docs/admin-console.md` the verb).

### `mixed upgrade(string owner, string *sources, int atom, object patchtool)`

SYSTEM()-gated. Drive a recompile cascade for the named source files under `owner`. Returns an array of failed paths -- empty on success -- or an error string when the upgrade cannot start or a precondition fails (one already running, patching still in progress, access denied, a missing source file, no existing issues); nil only signals a caller outside the gate. With `atom` nonzero the whole cascade runs atomically (all-or-nothing). A non-nil `patchtool` supplies `do_patch(obj)`, which the daemon applies to every live clone in an eager sweep of zero-delay callouts after the recompile, one object per tick (`docs/changing-a-running-system.md` describes the sweep).

The daemon's other entry points are internal (leaf generation is gated to the System auto).

## errord -- `src/usr/System/sys/errord.c`

The error manager has no application-facing query surface: its three functions (`runtime_error`, `atomic_error`, `compile_error`) are driver hooks, callable only by the kernel driver. Their dispatch, formatting, and logd persistence are documented in `docs/operations.md` (Logging and diagnostics) and `docs/debugging-applications.md` (Reading an error trace); the hook signatures live in `docs/kernel-reference/hook/driver`.

## logd -- `src/usr/System/sys/logd.c`

Persistent leveled logging into `/usr/System/log/system.log` (semantics and the operator surface: `docs/operations.md` Logging and diagnostics).

### `void log(int level, string msg)`

Public. Append a diagnostic line at `level` (the `LOG_DEBUG` .. `LOG_ERROR` constants). Lines below the threshold are dropped; admitted lines buffer and flush on a deferred call_out, which is what lets logging survive atomic execution. Never throws.

### `void log_report(string report)`

Gated to SYSTEM() or KERNEL() callers; errord's persistence path. Appends a pre-formatted report at ERROR level, bypassing the threshold.

### `void set_threshold(int level)` / `int query_threshold()`

The write is KERNEL()-gated (an ungated caller gets an error, not a silent nil) and errors on a level outside the valid range; the read is public. Default threshold is INFO. Operators reach these through the `log-level` verb.

## capabilityd -- `src/kernel/sys/capabilityd.c`

The capability store behind the platform's gating surfaces. `docs/capability.md` (The mechanism) is the owning document; the surface, for completeness:

- `int is_allowed(string capability, string principal)` -- public, silent membership read
- `void require_member(string capability, string principal)` -- throwing check, uniform denial message
- `void grant(string capability, string principal)` / `void revoke(string capability, string principal)` -- KERNEL()-gated mutation (an ungated caller gets an error)
- `string *query_principals(string capability)` / `string *query_capabilities()` -- public reads

## Index daemon -- `src/usr/Index/sys/index_daemon.c`

The logical-name registry behind `/lib/util/named.c` (`set_object_name` / `find_named`, catalogued in `docs/kernel-libraries.md`). Names are colon-delimited paths (`Domain:path:name`); registration is reserved to the named library, lookup is public.

### `atomic void set_name(object ob, string name)` / `atomic void clear_name(string name)`

Gated to `/lib/util/named` by `previous_program()`. Register or unregister a logical name. `set_name` errors on a malformed name (empty, leading `/`, empty segment) and on every collision class: the name taken by another object, an intermediate path segment already occupied by an object, or the name referring to a folder. Replacing an object's prior name is the caller's job -- the named library's `set_object_name` clears the old name before re-registering.

### `atomic void clear_name_for_object(object ob)`

KERNEL()-gated destruct-time cleanup: looks up and clears whatever name `ob` holds.

### `object query_object(string name)` / `string query_name(object ob)`

Public reads: name to object (nil if unregistered or the name is a folder), and object to name (nil if none).

### `mapping query_tree()` / `string *query_subdirs(varargs string path)` / `string *query_objects(varargs string path)`

Public introspection over the name tree: the full nested mapping, the folder names under `path`, and the object names under `path` (nil or absent `path` means the root).

## Where to next

- [`docs/code-lifecycle.md`](code-lifecycle.md): the object-manager event surface objectd records and the upgrade model upgraded drives.
- [`docs/operations.md`](operations.md): the logging pipeline and error dispatch these daemons implement.
- [`docs/capability.md`](capability.md): the capability model behind capabilityd's store.
- [`docs/kernel-reference/README.md`](kernel-reference/README.md): the "Where signatures live" router for every other kind of callable.
