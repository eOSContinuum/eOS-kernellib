# Observer lifecycle

Where dispatcher observers live and what happens to them from registration to end-of-life: the storage model, the lookup walk, the query and removal surfaces, and the persistence and eviction behavior. `dispatcher.md` is the dispatch reference -- what happens when a property is written (timing slots, veto, batching, cascade and cycle bounds, the observer-source contract). This page is the storage-and-lifecycle contract that dispatch relies on: where registrations are stored, how they are found, how they are inspected and mutated, and when they cease to exist.

The short form: an observer is a compiled Merry script appended to an ordered list in a property on the host object. The Merry daemon owns no registration state -- it owns the gates, the lookup, and a cache. Everything else in this contract follows from that placement.

## Registration

`register_observer(object ob, mixed path, string timing, string source)` on the Merry daemon:

- **Compile at registration.** `source` compiles immediately into a `/usr/Merry/data/merry` wrapper -- a light-weight object the slot stores, which permanently retains the source. A parse error surfaces at registration time, not at first fire.
- **Append.** The compiled object is appended to the slot list at `merry:on:<path>:<timing>` on `ob`. Registration order is firing order.
- **Gates.** The caller passes the registrar gate (`_check_registrar`), which passes exactly three shapes: a caller program under `/kernel/` (the console registry's verb helpers enter this way), a caller whose domain holds the `merry.registrar` capability, or a caller acting on a target in its own domain (the self-domain path). And `ob` must carry the property API (`/lib/util/properties`); the daemon refuses a target without it.
- **Timing.** One of `pre` / `main` / `post`, case-insensitive; nil defaults to `main`; anything else errors.

**Cross-property registration sugar**: `path` may be an array of paths. The source compiles once and the same compiled object is appended to each path's slot -- the observer fires once per observed-path write, with `$path` disambiguating which write triggered it. The storage model stays strictly per-`(path, timing)` slot: no stored record marks the N registrations as siblings, so fully retiring an array-registered observer takes one removal per slot. An empty array or a non-string entry is refused.

## Storage model

| Property | Holds |
|---|---|
| `merry:on:<path>:<timing>` | The slot: an ordered list of compiled observer objects. Written by `register_observer` and the removal surfaces. |
| `merry:on:<path>` | Read-side alias, treated as the `main` slot when the explicit form is absent. Registration always writes the explicit form; alias slots arise only from manual raw-property writes. |
| `merry:on-inherit:<path>:<timing>` | Boolean re-enable marker for the ancestry walk. An ordinary property the host sets on itself. |

- The slot lives on the **host's** property table, not in daemon state. Legacy single-value forms (a bare compiled object, a source string) normalize to one-element lists on read.
- The query and by-index removal surfaces resolve a slot the same way -- explicit form first, then the timing-less alias for `main` -- so the indices `query_observers` reports are always the indices `remove_observer` removes by, whichever property actually holds the slot. The coarse `unregister_observer` clears the explicit slot property; a hand-written alias slot is cleared by writing that property directly or drained through by-index removal, which resolves it.
- **Two `inherit` families, two types.** The walk marker `merry:on-inherit:<path>:<timing>` is a host-local boolean. The script-binding delegation properties `merry:inherit:<mode>:<signal>` (`merry-applications.md`) hold object pointers forming a delegation chain. Same word, different families; nothing is shared between them.
- **Write gating.** The slot and marker properties are themselves protected on the dispatched write path: a `set_property` of `merry:on:*` or `merry:on-inherit:*` re-applies the registrar gate and fails closed. `set_raw_property` remains the deliberate bypass -- bounded by object-reference access control and stated here as a limitation, not a guarantee.

## Lookup

Observer resolution walks the ur-ancestry (`query_parent()`) from the host upward, declarative-dominant with explicit re-enable: a level with local observers accumulates them and stops the walk unless it also sets the re-enable marker; observer-less levels are transparent. `dispatcher.md` "Ancestry walk" states the policy table and a worked example.

- **Single-parent contract.** `query_parent()` returns zero or one parent, so the walk is a chain, not a graph, and effective firing order is unambiguous: the host's slot order first, then each contributing ancestor's, in walk order. This contract is specified for the single-parent hierarchy only; if a future hierarchy extension introduces multiple parents, the walk's ordering and dominance semantics must be revisited explicitly rather than assumed.
- **Cache, and the invalidation truth.** The daemon caches resolved walks per `(object, path, timing)` for the dispatch path. Invalidation is broad: any registration, removal, or unregistration clears the entire cache map -- the deliberate minimal choice, not a targeted per-key removal (the invalidation helper's arguments are carried for a future targeted switch and are currently unused). The cache is a dispatch-path optimization only; the query surface below never reads it.

## Firing

Firing is the dispatcher's half of the contract: pre / main / post slots, error-as-veto at pre, cascade and cycle bounds, batch identity, and the `$this` / `$path` / `$new` / `$old` / `$timing` / `$seq` bindings -- see `dispatcher.md`. One lifecycle-relevant point restated here: `$this` binds the dispatch host, not the ancestor that owns the observer, when the walk lands an ancestor's observer.

## Query surface

Three public read-only LFUNs on the daemon. All are computed fresh from property tables -- never from the cache -- and all return descriptive data, never compiled observer references: a compiled object is invocable (a holder could evaluate it with fabricated arguments), while a description exposes nothing a same-domain raw-property read does not already expose. Reads are ungated, matching the `query_approved_registrars` precedent -- observer topology is world-readable to `/usr` code, the same read posture `capability.md` states for the capability table.

| LFUN | View | Returns |
|---|---|---|
| `query_observers(ob, path, timing)` | The local slot -- ground truth from the host's property table | Descriptions in slot order; positions are the indices `remove_observer` takes |
| `query_effective_observers(ob, path, timing)` | The ancestry-walk view -- what a dispatch would fire, marker effects included | `({ owner_object_name, description })` pairs in firing order |
| `query_observed_paths(ob)` | Enumeration of the object's local observed slots | `({ path, timing })` pairs in property-key order; alias slots report as `main`; re-enable markers are not included (they are not slots) |

A description is the compiled object's name plus the leading characters of its source, flattened to one line -- enough to tell entries in a slot apart without handing out the reference. `query_effective_observers` is the "why did (or didn't) it fire" debugging view: it mirrors the dispatch walk exactly, marker effects included, and is computed fresh so it reflects ground truth rather than cache state.

## Mutation

Three shapes, all gated identically through the registrar gate:

- `register_observer` -- append (above).
- `remove_observer(ob, path, timing, index)` -- remove one entry by its `query_observers` index. An out-of-range index throws: in a single coherence domain the query-then-remove window is small, and the throw makes staleness visible instead of silently removing the wrong entry. Removing the last entry deletes the slot property, matching the coarse clear's end state. By-index rather than by-identity because the query surface deliberately withholds compiled references -- an external caller has no identity to remove by.
- `unregister_observer(ob, path, timing)` -- the coarse clear: removes all observers at the triple by deleting the slot property. The start-fresh shape.

Operator access: the `observers` verb exposes all three query views, and `unregister-observer` exposes both removal shapes via an optional index argument -- `admin-console.md` "Dispatcher operator surface".

## Persistence and end-of-life

**Statedump survival.** A registration survives a snapshot + restore cycle end-to-end: the slot is ordinary property storage, the stored wrapper is a light-weight object living inside the host's dataspace and persisting with it -- retained source included -- and the compiled `/usr/Merry/merry/<md5>` program is recompiled from that source in memory on demand (the two-argument `compile_object` form; no on-disk `.c` file is written or read). `dispatcher.md` "Persistence" walks the guarantees element by element; the merry-app persistence phases prove the composition empirically.

**Host destruct.** Registrations live in the host's property table, so destructing the host destroys its registrations with it. The daemon holds no per-host registration state to clean up; residual cache entries for a destructed host are inert, and the cache clears wholesale on the next registration change anywhere.

**Compiled-program eviction.** The slot stores the `/usr/Merry/data/merry` wrapper, which permanently retains the observer's source. The daemon's node pool tracks only the compiled `/usr/Merry/merry/<md5>` program masters behind those wrappers: past the pool bound (four times the node budget of 256), an eviction pass keeps the most recently fired three-quarters and destructs the rest. Evicting a program invalidates no registration -- wrappers are never evicted, so no stale reference ever appears in a slot list. Eviction nils the wrapper's program pointer; the next fire lazily recompiles from the retained source and re-enters the program in the pool. The cost of eviction is one recompile at next fire -- a transient, not a correctness exposure. The merry-app eviction phase proves the full cycle: destruct a registered observer's compiled program directly via the per-node `suicide()` primitive -- the same call `clean_nodes` drives in bulk -- then snapshot, restore, write the observed path, and the observer still fires.

**Limitation -- ungated eviction.** Both eviction surfaces are public: `clean_nodes` (the bulk pass on the daemon, callable cross-domain by any `/usr` code) and the per-node `suicide()` on the compiled program itself (callable by anything holding a program reference, with no pool-bound trigger involved). The exposure is transient cost (forced recompiles), not correctness. Kernel-tier gating is a candidate hardening if forced eviction ever matters operationally.

## Verification

| Contract | Exercised by |
|---|---|
| Query views: local slot order, effective walk with owner labels, observed-path enumeration | merry-app `OBSERVER QUERY` phase |
| Array-path sugar: compiles once (slots share one object), fires once per observed path | merry-app `OBSERVER SUGAR` phase |
| By-index removal; out-of-range refusal; cross-domain caller refusal | merry-app `OBSERVER REMOVE` phase |
| Coarse clear: slot property deleted, nothing fires afterward | merry-app `OBSERVER CLEAR` phase |
| Eviction survival across snapshot + restore | merry-app `OBSERVER EVICT` phase |
| Console shapes: observed-path enumeration, `-effective` (and its path-required refusal), index-argument refusals | `scripts/verbsets/dispatcher-verbs.verbset` |
| Statedump survival of registrations | merry-app `PERSIST SETUP` / `PERSIST VERIFY` phases |

## See also

- `dispatcher.md` -- the dispatch reference: firing semantics, batching, veto, bounds, the observer-source contract, kernel-layer internals.
- `merry-applications.md` -- the script-binding mechanism observers are one application of; the `merry:inherit:*` delegation family.
- `admin-console.md` -- the operator verbs (`observers`, `register-observer`, `unregister-observer`).
- `capability.md` -- the `merry.registrar` capability and the gating model the mutation surface routes through.
- `runtime-platform-roadmap.md` -- where the observer surface sits on the platform roadmap.
