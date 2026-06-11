/*
 * SYS_MERRY: the Merry daemon.
 *
 * Maintains a cache registry of compiled LPC snippets in the form of
 * /merry/<md5> objects. Hosts the yacc parser (via parse::create from
 * /lib/util/fileparse + grammar/merry.y) and exposes a runtime-extensible
 * script-space registry (register_script_space / space::method() syntax).
 *
 * Also hosts the property-change dispatcher: observer registration
 * (capability-gated), the batching surface, and dispatch_set, the
 * single entry point property writes route through. The dispatcher's
 * full reference is docs/dispatcher.md.
 */

# include <status.h>
# include <type.h>
# include <kernel/kernel.h>
# include <Merry.h>

private inherit "/lib/util/ascii";
inherit parse "/lib/util/fileparse";
inherit "/usr/Merry/lib/merryapi";
private inherit "/lib/util/lpc";
inherit "/lib/util/named";

# define NODE_COUNT	256

# define HALFLIFE	30

mapping tick_usage;
mapping last_update;
mapping recent_ticks;
mapping node_map;

mapping script_spaces;
int     cleanup_stamp;

/*
 * Registrar capability-gate state. `approved_registrars` is the daemon-wide
 * allowlist of caller domains that may register observers / script-spaces
 * on objects outside their own domain; KERNEL programs always pass and
 * domain-match is the default allow path. Seeded with "System" +
 * "admin_console" at create(); extended via add_approved_registrar /
 * remove_approved_registrar (KERNEL-gated). Per-domain rather than
 * per-program — matches the cloud-server /usr/<domain>/ ownership model.
 *
 * `observer_cache` caches resolved observer lists. Broad
 * invalidation (clear the whole cache on any registration write) is the
 * deliberate minimal strategy; descendant-chain tracking is deferred
 * as an implementation choice.
 */
mapping approved_registrars;
mapping observer_cache;

/*
 * Batching surface state. `next_batch_id` is the daemon-wide
 * monotonically-increasing counter advanced at every batch boundary
 * (explicit and implicit) per the implicit-batch semantics; statedump-persistent so batch
 * identity survives restart. `batch_status` is a minimal stub for the
 * batch-status entries the batch-status contract calls for; keyed by batch-id, value is
 * ({ status, reason }). A future change-log surface will subsume this into the full change-log
 * surface (`query_changes_since`, retention pruning, etc.); the daemon-
 * local mapping here is enough for smoke verification and for the
 * dispatcher hooks to record outcomes against.
 */
int     next_batch_id;
mapping batch_status;

/*
 * TLS slot for the per-execution batch-context stack. Each entry is a
 * mapping { batch_id, atomic, opts, seq, cascade_depth } describing
 * one active batch frame; nested batches push/pop. Stack lifetime is
 * tied to the outermost call_limited frame (TLS reaps when that frame
 * returns), which gives "no batch active" the natural nil reading on
 * a fresh execution.
 *
 * The dispatcher adds TLS_CYCLE_CHAIN for cycle detection: a per-EXECUTION
 * stack of (object_name + ":" + path) keys appended at dispatch_set
 * entry and popped on exit. Persists across nested batches; resets
 * only when the outermost call_limited frame returns. String-keyed
 * (not (obj, path) tuple) so the `member()` predicate (from
 * /lib/util/lpc; LPC array intersection) is well-defined on string
 * equality. DGD has no `member_array` kfun in this build; the static
 * `member(item, arr)` helper from /lib/util/lpc is the equivalent.
 */
# define TLS_BATCH_STACK	"merry_batch_stack"
# define TLS_CYCLE_CHAIN	"merry_cycle_chain"

/*
 * Dispatcher state. max_cascade_depth caps total successful
 * property writes within a single batch per the cascade-depth bound; seeded to 32 at
 * create(), configurable via set_max_cascade_depth (KERNEL-gated).
 * Statedump-persistent.
 */
# define DEFAULT_MAX_CASCADE_DEPTH	32
int max_cascade_depth;

/*
 * Operator-facing verbose dispatch tracing. dispatch_trace is a daemon-wide flag
 * toggled by admin_console_ext's `dispatch-trace on|off` verb (routed
 * through ADMIN_CONSOLE_REGISTRY's KERNEL-tier helper) or set directly
 * via set_dispatch_trace (KERNEL-gated). When non-zero, _trace_dispatch
 * appends entry events to MERRY_LOG_FILE alongside the always-logged
 * cycle/cascade events from _log_dispatch. Statedump-persistent.
 */
int dispatch_trace;

/*
 * Dispatcher log path. The logging design calls for sysLog growth so the cycle
 * chain is recoverable post-mortem; rather than wire the global
 * sysLog stub (a cross-cutting concern), the dispatcher keeps a Merry-
 * local file logger used only by the dispatcher for cycle and cascade
 * events. A future logging facility can grow sysLog wholesale.
 */
# define MERRY_LOG_DIR	"/usr/Merry/log"
# define MERRY_LOG_FILE	"/usr/Merry/log/dispatch.log"

/* merry code begins 5 lines into the generated LPC file */
int query_line_offset() { return 5; }

string queryStateRoot() { return "Merry"; }

static
void create() {
   recent_ticks = ([ ]);
   last_update = ([ ]);
   tick_usage = ([ ]);

   script_spaces = ([ ]);
   approved_registrars = ([ "System": 1, "admin_console": 1 ]);
   observer_cache = ([ ]);

   next_batch_id = 1;
   batch_status = ([ ]);

   max_cascade_depth = DEFAULT_MAX_CASCADE_DEPTH;
   dispatch_trace = 0;

   node_map = ([ ]);
   compile_object("/usr/Merry/data/merry");
   compile_object("/usr/Merry/data/mcontext");

   set_object_name("Merry");

   /* /usr/Merry/{tmp,merry,merry/cleaned,log} are lazy-write targets.
    * /tmp/merry holds the yacc-generated parser scratch object;
    * /merry/<md5>.c holds the compiled wrapper objects produced by
    * data/merry::create(); /merry/cleaned/ catches files renamed
    * by merrynode::do_suicide during LRU eviction; /log/ holds the
    * dispatcher log. None exist in a fresh runtime tree, so
    * create them here -- catch the EEXIST shape so warm restarts
    * don't error. */
   catch(make_dir("/usr/Merry/tmp"));
   catch(make_dir("/usr/Merry/merry"));
   catch(make_dir("/usr/Merry/merry/cleaned"));
   catch(make_dir(MERRY_LOG_DIR));

   parse :: create("/usr/Merry/tmp/merry", "/usr/Merry/grammar/merry.y");
}

/*
 * _check_registrar: the registrar capability gate. Called by register_script_space,
 * unregister_script_space, and register_observer. Throws unless caller is
 * /kernel/* (KERNEL programs always trusted), OR caller's domain is in
 * approved_registrars, OR caller's domain matches target_name's domain.
 * The domain-match path lets each daemon register on its own objects
 * without needing to be in the approved set.
 *
 * caller_program MUST be captured at the public-LFUN entry via
 * previous_program() and threaded in; calling previous_program() inside
 * this helper would return "/usr/Merry/sys/merry" (the daemon program
 * containing the public LFUN), not the true caller program.
 */
private
void _check_registrar(string caller_program, string target_name) {
   string caller_domain, target_domain;

   if (!caller_program) {
      error("MERRY: nil caller_program in _check_registrar (call from interactive?)");
   }
   if (sscanf(caller_program, "/kernel/%*s") != 0) {
      return;
   }
   if (sscanf(caller_program, "/usr/%s/", caller_domain) == 0) {
      error("MERRY: unrecognized caller program " + caller_program);
   }
   if (approved_registrars[caller_domain]) {
      return;
   }
   if (target_name && sscanf(target_name, "/usr/%s/", target_domain) != 0 &&
       target_domain == caller_domain) {
      return;
   }
   error("MERRY: caller domain " + caller_domain +
         " not authorized to register on " + (target_name ? target_name : "(nil)"));
}

/*
 * _check_property_bearer: register/unregister target validation. The
 * observer store IS the target's property table; an object that does
 * not inherit the property API silently ignores the call_other writes
 * (call_other on a missing function returns nil), which reads as a
 * successful registration that stores nothing. Erroring here turns
 * that silent no-op into a caller-visible refusal.
 */
private
void _check_property_bearer(object ob) {
   if (!function_object("set_raw_property", ob)) {
      error("MERRY: target " + ::object_name(ob) +
            " does not carry the property API (inherit /lib/util/properties)");
   }
}

/*
 * _invalidate_observer_cache: broad invalidation as the deliberate minimal choice.
 * Any observer-property write clears the whole cache. Args carried for
 * future descendant-chain-tracking switch; currently unused.
 */
private
void _invalidate_observer_cache(object ob, string path, string timing) {
   observer_cache = ([ ]);
}

/*
 * register_observer: procedural sugar for declarative observer
 * writes. Compiles the source at registration time (a deliberate choice:
 * store compiled merry-script OBJECTS, not source strings, so
 * the dispatcher's per-fire path avoids recompilation and syntax
 * errors surface at registration rather than first-fire). Reads the
 * existing list, normalizes string + array + object forms, appends
 * the new compiled object, writes back via host's set_raw_property.
 *
 * set_raw_property is used (not set_property) so the registration
 * write does not itself trigger the dispatcher on the merry:on:* path
 * -- the dispatcher would find no observers and recurse harmlessly,
 * but the explicit raw write keeps the registration path independent
 * of dispatch infrastructure and avoids spurious batch-context entries
 * during boot when many registrations land in sequence.
 *
 * Capability-gated per the registrar capability gate; per-timing independent per the per-timing-independence contract.
 * Writes the explicit form `merry:on:<path>:<timing>`; the alias form
 * `merry:on:<path>` (timing-less = implicit main) is accepted on read
 * by find_observers as the minimal lookup choice.
 */
void register_observer(object ob, string path, string timing, string source) {
   string caller_program, prop_name, low_timing;
   object compiled;
   mixed existing;
   mixed *list;

   caller_program = previous_program();
   if (!ob) {
      error("register_observer: nil host object");
   }
   _check_registrar(caller_program, ::object_name(ob));
   _check_property_bearer(ob);

   low_timing = timing ? lower_case(timing) : "main";
   if (low_timing != "pre" && low_timing != "main" && low_timing != "post") {
      error("register_observer: timing must be one of {pre, main, post}; got \"" +
            low_timing + "\"");
   }
   prop_name = "merry:on:" + path + ":" + low_timing;

   compiled = ::new_object("/usr/Merry/data/merry", source);

   existing = ob->query_raw_property(prop_name);
   if (typeof(existing) == T_ARRAY) {
      list = existing;
   } else if (typeof(existing) == T_STRING || typeof(existing) == T_OBJECT) {
      list = ({ existing });
   } else {
      list = ({ });
   }
   list += ({ compiled });
   ob->set_raw_property(prop_name, list);

   _invalidate_observer_cache(ob, path, low_timing);
}

/*
 * unregister_observer: operator-console mutation surface paired with register_observer
 * for the admin_console_ext `unregister-observer` verb. Removes ALL
 * observers at (ob, path, timing) by clearing the merry:on:<path>:<timing>
 * property. Finer-grained removal (by source string or by compiled-object
 * identity) is future work -- the compiled-object identity
 * path is the more tractable shape (source strings don't survive compile)
 * but adds API surface beyond diagnostic-tier needs.
 * Capability-gated identically to register_observer via _check_registrar.
 */
void unregister_observer(object ob, string path, string timing) {
   string caller_program, prop_name, low_timing;

   caller_program = previous_program();
   if (!ob) {
      error("unregister_observer: nil host object");
   }
   _check_registrar(caller_program, ::object_name(ob));
   _check_property_bearer(ob);

   low_timing = timing ? lower_case(timing) : "main";
   if (low_timing != "pre" && low_timing != "main" && low_timing != "post") {
      error("unregister_observer: timing must be one of {pre, main, post}; got \"" +
            low_timing + "\"");
   }
   prop_name = "merry:on:" + path + ":" + low_timing;

   ob->set_raw_property(prop_name, nil);

   _invalidate_observer_cache(ob, path, low_timing);
}

/*
 * Capability-set extension trio per the capability-gate design. add and
 * remove are KERNEL-gated; query is public read-only. Pattern mirrors
 * /kernel/sys/access_daemon.c set_global_access -- daemon-local mapping,
 * statedump-persistent, gated at the entry point.
 */
void add_approved_registrar(string domain) {
   if (!KERNEL()) {
      error("add_approved_registrar: not callable from outside /kernel");
   }
   approved_registrars[domain] = 1;
}

void remove_approved_registrar(string domain) {
   if (!KERNEL()) {
      error("remove_approved_registrar: not callable from outside /kernel");
   }
   approved_registrars[domain] = nil;
}

string *query_approved_registrars() {
   return map_indices(approved_registrars);
}

/*
 * Batching surface. Implements the hybrid batching design with the
 * atomic-mode opt-in. Two public LFUNs (`batch`, `batched_set`)
 * and a small set of internal helpers exposed for the dispatcher and
 * the future change-log surface to weave into. Per the capability-gate design (LFUN-entry gating only) the
 * capability gate is LFUN-only at registration entry points -- batching
 * writes through `set_property` and is not gated here.
 *
 * Per-execution context lives in TLS under TLS_BATCH_STACK as a stack of
 * { batch_id, atomic, opts, seq, cascade_depth } mappings. Nested batches
 * push; exit pops. The the cycle-detection design puts the cycle-chain at a
 * separate TLS slot (the dispatcher owns); the batch-stack here tracks only what
 * a batch needs to identify and account for itself.
 */

private
mapping _make_batch_context(int batch_id, int atomic_mode, mapping opts) {
   return ([
      "batch_id":      batch_id,
      "atomic":        atomic_mode,
      "opts":          opts ? opts : ([ ]),
      "seq":           0,
      "cascade_depth": 0,
   ]);
}

private
int _push_batch_context(int atomic_mode, mapping opts) {
   mapping *stack;
   int batch_id;

   stack = tls_get(TLS_BATCH_STACK);
   if (!stack) {
      stack = ({ });
   }
   batch_id = next_batch_id ++;
   stack += ({ _make_batch_context(batch_id, atomic_mode, opts) });
   tls_set(TLS_BATCH_STACK, stack);
   return batch_id;
}

private
void _pop_batch_context() {
   mapping *stack;
   int n;

   stack = tls_get(TLS_BATCH_STACK);
   n = stack ? sizeof(stack) : 0;
   if (n == 0) {
      error("MERRY: _pop_batch_context called with empty batch stack");
   }
   if (n == 1) {
      tls_set(TLS_BATCH_STACK, nil);
   } else {
      tls_set(TLS_BATCH_STACK, stack[.. n - 2]);
   }
}

/*
 * Records the batch-status entry per the batch-status contract. Stub shape: writes to the
 * daemon-local `batch_status` mapping. The future change-log surface will replace this with the
 * full change-log surface (`query_changes_since` + retention + per-host
 * pruning). The six-status enum: completed,
 * cascade-aborted, cycle-detected, pre-vetoed, main-aborted, post-aborted.
 *
 * Check-before-overwrite: the dispatcher records a specific
 * abort category (e.g., "pre-vetoed") inside dispatch_set when an
 * observer throws; the outer batch() / batched_set() then catches the
 * propagated error and calls _record_batch_status again with a less-
 * specific "main-aborted" -- without the guard, the dispatcher's
 * category would be clobbered. First-write-wins preserves the
 * dispatcher's specific category through the propagation.
 */
private
void _record_batch_status(int batch_id, string status, mixed reason) {
   if (batch_status[batch_id]) {
      return;
   }
   batch_status[batch_id] = ({ status, reason });
}

/*
 * Public read-only accessors over the batch-stack TLS slot. The dispatcher calls
 * _current_batch_id() to decide whether to allocate an implicit batch on
 * an unbatched property write; the dispatcher and the future change-log surface read _current_batch_context()
 * to track cascade-depth and to fetch the active batch-id when writing
 * change-log tuples, advancing _current_batch_seq_advance() per tuple.
 */
int _current_batch_id() {
   mapping *stack;
   int n;

   stack = tls_get(TLS_BATCH_STACK);
   n = stack ? sizeof(stack) : 0;
   return n ? stack[n - 1]["batch_id"] : 0;
}

mapping _current_batch_context() {
   mapping *stack;
   int n;

   stack = tls_get(TLS_BATCH_STACK);
   n = stack ? sizeof(stack) : 0;
   return n ? stack[n - 1] : nil;
}

int _current_batch_seq_advance() {
   mapping ctx;
   int seq;

   ctx = _current_batch_context();
   if (!ctx) {
      error("MERRY: _current_batch_seq_advance with no active batch");
   }
   seq = ctx["seq"];
   ctx["seq"] = seq + 1;
   return seq;
}

mixed *_query_batch_status(int batch_id) {
   return batch_status[batch_id];
}

/*
 * Hooks for the implicit-single-mutation path per the
 * implicit-batch semantics. The dispatcher calls
 * _enter_implicit_batch() at the top of `set_property` when no batch is
 * already active: a fresh non-atomic batch-id is allocated so the
 * unbatched mutation lands in the change-log with seq=0 under its own
 * batch identity. When nested inside an existing batch (explicit or
 * implicit), returns 0 to signal the dispatcher should re-use the
 * already-active batch.
 *
 * _exit_implicit_batch must be called with the returned batch-id; a 0
 * argument is a no-op so the dispatcher's exit path is uniform across
 * outermost-vs-nested cases.
 */
int _enter_implicit_batch() {
   if (_current_batch_id() != 0) {
      return 0;
   }
   return _push_batch_context(0, nil);
}

void _exit_implicit_batch(int batch_id, string status, mixed reason) {
   if (batch_id == 0) {
      return;
   }
   _record_batch_status(batch_id, status, reason);
   _pop_batch_context();
}

/*
 * Body helpers. The non-atomic pair runs the batch body directly; the
 * atomic pair runs inside DGD's atomic{} via the function-level `atomic`
 * modifier so a throw rolls back all mutations made during the body
 * (per the atomic-mode opt-in semantics). The atomic-mode status-entry write
 * happens INSIDE the atomic function so a rollback also erases it,
 * matching the "atomic batches that abort leave no trace" contract.
 */

private
mixed _run_callable(object obj, string func, mixed *args) {
   return ::call_other(obj, func, args...);
}

private
void _run_kv_writes(object obj, mapping kv_map) {
   string *keys;
   int i, n;

   keys = map_indices(kv_map);
   n = sizeof(keys);
   for (i = 0; i < n; i ++) {
      /*
       * set_property routes through dispatch_set, which owns
       * seq-advance and observer firing around the actual write. The
       * earlier explicit _current_batch_seq_advance call was removed
       * so seq advances exactly once per dispatched write whether
       * the path is batched or unbatched.
       */
      ::call_other(obj, "set_property", keys[i], kv_map[keys[i]]);
   }
}

private atomic
mixed _atomic_run_callable(object obj, string func, mixed *args, int batch_id) {
   mixed result;

   result = _run_callable(obj, func, args);
   _record_batch_status(batch_id, "completed", nil);
   return result;
}

private atomic
void _atomic_run_kv_writes(object obj, mapping kv_map, int batch_id) {
   _run_kv_writes(obj, kv_map);
   _record_batch_status(batch_id, "completed", nil);
}

/*
 * batch: the function-reference batching API. Invokes
 * obj->func(args...) inside a fresh batch context; atomic-mode opt-in
 * via opts mapping via the atomic-mode opt-in (`([ "atomic": 1 ])`). Non-atomic is
 * the catch'd-error default: on throw, a "main-aborted" status
 * entry persists per the batch-status contract and the error propagates to the caller's
 * catch{}. In atomic-mode, DGD rolls back both mutations and the status
 * entry (the atomic batch leaves no trace per the atomic-mode opt-in).
 *
 * Not callable from Merry observer source by design -- the SANDBOX
 * whitelist in merrynode.c does not expose this verb because Merry
 * has no function-reference syntax.
 */
mixed batch(object obj, string func, mixed *args, varargs mapping opts) {
   int batch_id, atomic_mode;
   mixed result, err;

   if (!obj) {
      error("batch: nil object");
   }
   if (!func) {
      error("batch: nil function name");
   }
   if (typeof(args) != T_ARRAY) {
      error("batch: args must be an array");
   }
   atomic_mode = (opts && opts["atomic"]) ? 1 : 0;

   batch_id = _push_batch_context(atomic_mode, opts);

   if (atomic_mode) {
      err = catch(result = _atomic_run_callable(obj, func, args, batch_id));
   } else {
      err = catch(result = _run_callable(obj, func, args));
   }

   if (err) {
      if (!atomic_mode) {
         _record_batch_status(batch_id, "main-aborted", err);
      }
      _pop_batch_context();
      error(err);
   }

   if (!atomic_mode) {
      _record_batch_status(batch_id, "completed", nil);
   }
   _pop_batch_context();
   return result;
}

/*
 * batched_set: the mapping-write batching API. Writes the kv_map's
 * key/value pairs to obj via set_property; all writes share a single
 * batch-id with sequential seq values starting at 0 (per the implicit-batch semantics).
 * Atomic-mode opt-in identical to batch() per the atomic-mode opt-in.
 *
 * Callable from Merry observer source (via the BatchedSet merryfun in
 * merrynode.c) because the mapping-arg signature needs no function
 * reference; arguments compose inline.
 */
mixed batched_set(object obj, mapping kv_map, varargs mapping opts) {
   int batch_id, atomic_mode;
   mixed err;

   if (!obj) {
      error("batched_set: nil object");
   }
   if (typeof(kv_map) != T_MAPPING) {
      error("batched_set: kv_map must be a mapping");
   }
   atomic_mode = (opts && opts["atomic"]) ? 1 : 0;

   batch_id = _push_batch_context(atomic_mode, opts);

   if (atomic_mode) {
      err = catch(_atomic_run_kv_writes(obj, kv_map, batch_id));
   } else {
      err = catch(_run_kv_writes(obj, kv_map));
   }

   if (err) {
      if (!atomic_mode) {
         _record_batch_status(batch_id, "main-aborted", err);
      }
      _pop_batch_context();
      error(err);
   }

   if (!atomic_mode) {
      _record_batch_status(batch_id, "completed", nil);
   }
   _pop_batch_context();
   return nil;
}

/*
 * The dispatcher. dispatch_set wraps each property write with
 * pre/main/post observer firing per the pre->write->main->post ordering contract, cascade-depth bound per
 * the cascade-bound design, cycle detection per the cycle-detection design, and implicit
 * batch wrapping per the implicit-batch semantics. Called from /lib/util/properties.c
 * set_property when MERRY is loaded; the raw write step uses
 * obj->set_raw_property to bypass re-entry.
 *
 * Configuration accessors per the dispatcher configuration surface. set is KERNEL-gated like the
 * approved-registrar mutators; query is public read-only.
 */
void set_max_cascade_depth(int n) {
   if (!KERNEL()) {
      error("set_max_cascade_depth: not callable from outside /kernel");
   }
   if (n < 1) {
      error("set_max_cascade_depth: bound must be >= 1");
   }
   max_cascade_depth = n;
}

int query_max_cascade_depth() {
   return max_cascade_depth;
}

/*
 * Dispatch-trace accessors. set is KERNEL-gated for symmetry with
 * set_max_cascade_depth; query is public read-only. The flag is non-zero
 * to enable verbose trace logging; _trace_dispatch consults this flag
 * and emits an entry-event line per dispatch_set call. The cycle and
 * cascade events go through _log_dispatch unconditionally (they're
 * always informative); _trace_dispatch is the optional fine-grain
 * surface for operator-driven troubleshooting.
 */
void set_dispatch_trace(int flag) {
   if (!KERNEL()) {
      error("set_dispatch_trace: not callable from outside /kernel");
   }
   dispatch_trace = flag ? 1 : 0;
}

int query_dispatch_trace() {
   return dispatch_trace;
}

/*
 * Dispatcher helpers. _cycle_chain_get / _cycle_chain_set manage
 * the per-execution cycle chain at TLS_CYCLE_CHAIN; the chain is an
 * array of string keys of the form `<object_name>:<path>` so
 * the static `member()` predicate (from /lib/util/lpc; LPC array-
 * intersection on strings) is well-defined.
 *
 * _resolve_observer normalizes find_observers' return values to
 * compiled merry-script objects: T_OBJECT passes through; T_STRING
 * (legacy source-string property values) is compiled lazily. Future
 * registrations always store compiled objects by design
 * to register_observer, but the dispatcher remains tolerant.
 *
 * _find_observers_cached is the the cache-population helper.
 * Cache key is `<object_name>:<path>:<timing>`; on miss it calls
 * the static find_observers from merryapi and stores the resolved
 * list. The existing _invalidate_observer_cache clears the
 * whole map on observer-property writes, so the cache key shape
 * does not need to be partition-friendly.
 *
 * _log_dispatch is the the dispatcher-logging note Merry-local file logger for cycle
 * and cascade events. Appends one line per event to MERRY_LOG_FILE;
 * catch'd so log-write failures do not propagate into dispatch.
 */
private
string _cycle_key(object obj, string path) {
   return ::object_name(obj) + ":" + path;
}

private
string *_cycle_chain_get() {
   string *chain;

   chain = tls_get(TLS_CYCLE_CHAIN);
   return chain ? chain : ({ });
}

private
void _cycle_chain_push(string key) {
   string *chain;

   chain = _cycle_chain_get();
   tls_set(TLS_CYCLE_CHAIN, chain + ({ key }));
}

private
void _cycle_chain_pop() {
   string *chain;
   int n;

   chain = _cycle_chain_get();
   n = sizeof(chain);
   if (n == 0) {
      error("MERRY: _cycle_chain_pop with empty chain");
   }
   if (n == 1) {
      tls_set(TLS_CYCLE_CHAIN, nil);
   } else {
      tls_set(TLS_CYCLE_CHAIN, chain[.. n - 2]);
   }
}

private
object _resolve_observer(mixed val) {
   if (typeof(val) == T_OBJECT) {
      return val;
   }
   if (typeof(val) == T_STRING) {
      return ::new_object("/usr/Merry/data/merry", val);
   }
   error("MERRY: observer entry has unexpected type " + (string) typeof(val));
}

private
mixed *_find_observers_cached(object obj, string path, string timing) {
   string key;
   mixed *cached;

   key = ::object_name(obj) + ":" + path + ":" + timing;
   cached = observer_cache[key];
   if (cached) {
      return cached;
   }
   cached = find_observers(obj, path, timing);
   observer_cache[key] = cached;
   return cached;
}

private
void _log_dispatch(string msg) {
   catch {
      mixed *info;
      int size;

      info = ::file_info(MERRY_LOG_FILE);
      size = info ? info[0] : 0;
      ::write_file(MERRY_LOG_FILE, msg + "\n", size);
   }
}

/*
 * _trace_dispatch is the optional fine-grain trace surface gated by
 * dispatch_trace. Identical write path to _log_dispatch but elides the
 * file I/O entirely when the flag is unset (the common case). The operator surface
 * threads this into dispatch_set entry; future work may add
 * batch-entry, observer-fire, and cascade-depth-increment trace sites.
 */
private
void _trace_dispatch(string msg) {
   if (!dispatch_trace) {
      return;
   }
   _log_dispatch("trace: " + msg);
}

/*
 * _fire_timing_slot: iterates the observers for a (obj, path, timing)
 * triple in LPC list order (the documented firing order); each evaluate is wrapped in
 * catch so the first throw halts the slot and propagates per the pre-veto contract.
 * The args mapping carries the change context (path, new value, old
 * value, timing, host) into the merry-script's args TLS, accessible
 * from the source as $path / $new / $old / $timing / $this.
 */
private
void _fire_timing_slot(object obj, string path, string timing,
                       mapping args) {
   mixed *raw;
   object compiled;
   int i, n;
   mixed err;

   raw = _find_observers_cached(obj, path, timing);
   n = sizeof(raw);
   for (i = 0; i < n; i ++) {
      compiled = _resolve_observer(raw[i]);
      /*
       * The compiled merry script's `merry(mode, signal, label)` LFUN
       * is a switch-on-label; only the "virgin" case carries the
       * source body (other labels are resume-after-$delay entry
       * points). Pass nil so evaluate defaults the label to "virgin"
       * and the source executes from the top.
       */
      err = catch(compiled->evaluate(obj, path, timing, args, nil));
      if (err) {
         error(err);
      }
   }
}

/*
 * dispatch_set: Pre->write->main->post sequencing around a
 * property write. Bounded by the cascade-bound design cascade depth (per-batch
 * counter) and cycle detection (per-execution chain). Wraps unbatched
 * mutations in an implicit batch per the implicit-batch semantics. Records batch-status
 * per the batch-status contract + the atomic-mode opt-in on abort categories; check-before-overwrite
 * so the dispatcher's specific category survives if batch() / batched_set
 * subsequently catches the same error.
 *
 * Args carried into observer source: $path (string), $new (mixed),
 * $old (mixed), $timing (string), $this (object). The merry source
 * can mutate further state via Set/BatchedSet; those recursive writes
 * re-enter dispatch_set bounded by cascade + cycle checks.
 */
mixed dispatch_set(object obj, string path, mixed val) {
   int implicit_batch_id;
   mapping ctx;
   string cycle_key;
   string *chain;
   mapping args;
   mixed old_val;
   mixed err;
   int local_seq;

   if (!obj) {
      error("dispatch_set: nil host object");
   }
   if (!path) {
      error("dispatch_set: nil path");
   }

   _trace_dispatch("dispatch_set " + ::object_name(obj) + ":" + path);

   implicit_batch_id = _enter_implicit_batch();

   ctx = _current_batch_context();
   if (!ctx) {
      error("MERRY: dispatch_set with no batch context after implicit-enter");
   }

   /*
    * The cascade-depth bound. Counter is depth-shaped (incremented
    * at dispatch entry after the bound check, decremented at exit on
    * BOTH success and failure paths) so a flat batch with many
    * legitimate writes does not hit the cap, but a deep recursive
    * chain does. The check compares the CURRENT depth (before this
    * dispatch's increment) against max -- if the parent's depth has
    * already reached max, this nested dispatch refuses. The the depth-vs-count distinction
    * note "increments only on successful completion" is reconciled by
    * decrementing on failure too: vetoed mutations roll the counter
    * back rather than leave it artificially inflated.
    */
   if (ctx["cascade_depth"] >= max_cascade_depth) {
      string emsg;

      emsg = "merry: cascade depth " + max_cascade_depth +
             " exceeded at " + ::object_name(obj) + ":" + path;
      _log_dispatch(emsg);
      _record_batch_status(ctx["batch_id"], "cascade-aborted", emsg);
      if (implicit_batch_id) {
         _exit_implicit_batch(implicit_batch_id, "cascade-aborted", emsg);
      }
      error(emsg);
   }

   /*
    * Cycle detection. Check before push so the firing
    * (obj, path) is not its own first-occurrence false-positive.
    */
   cycle_key = _cycle_key(obj, path);
   chain = _cycle_chain_get();
   if (member(cycle_key, chain)) {
      string emsg;

      emsg = "merry: observer cycle detected at " + cycle_key;
      _log_dispatch("cycle-detected: " + cycle_key +
                    " already in chain " + implode(chain, " -> "));
      _record_batch_status(ctx["batch_id"], "cycle-detected", emsg);
      if (implicit_batch_id) {
         _exit_implicit_batch(implicit_batch_id, "cycle-detected", emsg);
      }
      error(emsg);
   }

   ctx["cascade_depth"] = ctx["cascade_depth"] + 1;
   _cycle_chain_push(cycle_key);

   /*
    * Capture old value before write; observers see both. seq advances
    * once per dispatched write (the earlier _run_kv_writes seq advance
    * is removed in this commit -- seq now lives entirely in dispatch_set
    * so batched + unbatched mutations both advance through the same
    * counter).
    */
   old_val = obj->query_raw_property(path);
   local_seq = _current_batch_seq_advance();

   args = ([
      "path":   path,
      "new":    val,
      "old":    old_val,
      "timing": nil,
      "seq":    local_seq,
   ]);

   /*
    * pre -> write -> main -> post per the pre->write->main->post ordering contract. Per the pre-veto and atomic-mode contracts:
    * any throw halts the current slot, records the timing-specific
    * abort category, and re-raises. Post-write slots do not retry
    * the failed slot; the contract is halt-and-propagate.
    */
   args["timing"] = "pre";
   err = catch(_fire_timing_slot(obj, path, "pre", args));
   if (err) {
      ctx["cascade_depth"] = ctx["cascade_depth"] - 1;
      _record_batch_status(ctx["batch_id"], "pre-vetoed", err);
      _cycle_chain_pop();
      if (implicit_batch_id) {
         _exit_implicit_batch(implicit_batch_id, "pre-vetoed", err);
      }
      error(err);
   }

   obj->set_raw_property(path, val);

   /*
    * Observer-property writes invalidate the cache so the next
    * dispatch sees the new registration. The _invalidate_observer_cache
    * is broad (clears the whole map); a dispatch on a non-observer
    * property does not invalidate.
    */
   if (sscanf(path, "merry:on:%*s") != 0 ||
       sscanf(path, "merry:on-inherit:%*s") != 0) {
      _invalidate_observer_cache(obj, path, nil);
   }

   args["timing"] = "main";
   err = catch(_fire_timing_slot(obj, path, "main", args));
   if (err) {
      ctx["cascade_depth"] = ctx["cascade_depth"] - 1;
      _record_batch_status(ctx["batch_id"], "main-aborted", err);
      _cycle_chain_pop();
      if (implicit_batch_id) {
         _exit_implicit_batch(implicit_batch_id, "main-aborted", err);
      }
      error(err);
   }

   args["timing"] = "post";
   err = catch(_fire_timing_slot(obj, path, "post", args));
   if (err) {
      ctx["cascade_depth"] = ctx["cascade_depth"] - 1;
      _record_batch_status(ctx["batch_id"], "post-aborted", err);
      _cycle_chain_pop();
      if (implicit_batch_id) {
         _exit_implicit_batch(implicit_batch_id, "post-aborted", err);
      }
      error(err);
   }

   /*
    * Successful completion: pop cascade-depth (decrement matches the
    * pre-pre-fire increment so the counter is depth-shaped), pop
    * cycle chain, exit implicit batch with "completed". For explicit
    * batches the outer batch() / batched_set() owns the batch-status
    * entry; the check-before-overwrite guard in _record_batch_status
    * preserves any specific abort category an inner dispatch set.
    */
   ctx["cascade_depth"] = ctx["cascade_depth"] - 1;
   _cycle_chain_pop();
   if (implicit_batch_id) {
      _exit_implicit_batch(implicit_batch_id, "completed", nil);
   }
   return val;
}

void register_script_space(string space, object ob) {
   string caller_program;

   caller_program = previous_program();
   space = lower_case(space);
   if (space == "merry") {
      error("cannot register the merry script space");
   }
   _check_registrar(caller_program, ::object_name(ob));
   if (!script_spaces) {
      script_spaces = ([ ]);
   }
   script_spaces[space] = ob;
}

void unregister_script_space(string space) {
   string caller_program;
   object existing;

   caller_program = previous_program();
   space = lower_case(space);
   if (space == "merry") {
      error("cannot unregister the merry script space");
   }
   if (!script_spaces) {
      script_spaces = ([ ]);
   }
   existing = script_spaces[space];
   if (existing == nil) {
      error("no such script space registered");
   }
   _check_registrar(caller_program, ::object_name(existing));
   script_spaces[space] = nil;
}

object query_script_space(string space) {
   space = lower_case(space);
   if (space == "merry") {
      return this_object();
   }
   if (!script_spaces) {
      script_spaces = ([ ]);
   }
   return script_spaces[space];
}

string *query_script_space_indices()
{
   return map_indices(script_spaces) - ({ "merry" });
}

void clear_script_spaces()
{
   script_spaces = ([ "merry": this_object() ]);
}

void add_script_space()
{
   script_spaces["#NEW#"] = this_object();
}

mapping query_script_space_mapping() {
   return script_spaces;
}

atomic
void new_merry_node(object program) {
   if (previous_program() == "/usr/Merry/data/merry") {
      node_map[program] = TRUE;
      if (map_sizeof(node_map) > 4 * NODE_COUNT) {
	 mixed *callouts;

	 callouts = status(this_object())[O_CALLOUTS];

	 if (!sizeof(callouts)) {
	    call_out("clean_nodes", 0);
	 }
      }
   }
}

atomic
void clean_nodes(varargs int all) {
   mapping sort;
   object *nodes;
   int ix, i;

   sysLog("MERRY: clearing " + (all ? "all" :
					(map_sizeof(node_map) - 3 * NODE_COUNT)) +
		  " nodes...");

   sort = ([ ]);

   nodes = map_indices(node_map) - ({ nil });

   if (!all) {
      for (i = 0; i < sizeof(nodes); i ++) {
	 ix = -nodes[i]->query_mru_stamp();
	 while (sort[ix]) {
	    ix --;
	 }
	 sort[ix] = nodes[i];
      }
      nodes = map_values(sort);
      nodes = nodes[NODE_COUNT * 3..];
   }
   for (i = 0; i < sizeof(nodes); i ++) {
      nodes[i]->suicide();

      /*
       * If clean_nodes() gets called repeatedly, don't let it start a
       * call_out() on the same object repeatedly.  We hit the callout roof
       * real fast that way.  So clean up the object in the node_map even
       * though it hasn't self-destructed yet.  We just assume it will, soon.
       */
      node_map[nodes[i]] = nil;
   }
   cleanup_stamp = time();
}

mixed **parse_merry(string str) {
   int    i;
   string *lines;
   mixed  *res, *tree;

   lines = explode(str, "\n");
   for (i = 0; i < sizeof(lines); i ++) {
      if (strlen(lines[i]) && lines[i][0] == '#') {
	 error("you can't begin a line with # in merry");
      }
   }
   res = ::parse_string(str);
   if (!res) {
      error("oops... internal merry grammar error");
   }
   tree = res[0];
   if (sizeof(res) == 1) {
      /* there was a compilation error */
      int line, i;

      line = 1;
      for (i = 0; i < sizeof(tree); i ++) {
	 if (typeof(tree[i]) == T_STRING) {
	    line += (sizeof(explode("\n" + tree[i] + "\n", "\n"))-1);
	 }
      }
      error("merry parse error around line: " + line);
   }
   return res;
}

mixed *raw_parse(string str) {
   return ::parse_string(str);
}

int Int(mixed val) {
   switch(typeof(val)) {
   case T_NIL:
      return 0;
   case T_INT:
      return val;
   case T_FLOAT:
      return (int) val;
   case T_STRING:
      if (sscanf(val, "%d", val)) {
	 return val;
      }
      break;
   }
   error("cannot convert value to integer");
}

float Flt(mixed val) {
   switch(typeof(val)) {
   case T_NIL:
      return 0.0;
   case T_INT:
      return (float) val;
   case T_FLOAT:
      return val;
   case T_STRING:
      if (sscanf(val, "%f", val)) {
	 return val;
      }
      break;
   }
   error("cannot convert value to float");
}


private
void shuffle(string id) {
   int stamp, now;

   now = time();

   stamp = Int(last_update[id]);
   if (stamp > 0 && stamp < now) {
      float dT;

      dT = (float) (now - stamp);
      tick_usage[id] = (int)
	 (pow(0.5, dT / (float) HALFLIFE) * Flt(tick_usage[id]) +
	  pow(0.5, dT / (float) HALFLIFE) * Flt(recent_ticks[id]));
      recent_ticks[id] = nil;
      if (tick_usage[id] == 0) {
	 tick_usage[id] = nil;
      }
   }
   last_update[id] = now;
}

void update_resource(object home, string signal, string mode, string label,
		     int ticks) {
   /* profiler hook; body left empty pending a logger story */
}

mapping *query_mappings() {
   return ({ tick_usage, recent_ticks, last_update });
}

mapping query_tick_usage() {
   string *ix;
   int i;

   ix = map_indices(tick_usage);
   for (i = 0; i < sizeof(ix); i ++) {
      shuffle(ix[i]);
   }
   return tick_usage;
}

mixed query_property(string prop) {
   if (prop) {
      string space;

      prop = lower_case(prop);

      if (sscanf(prop, "script-spaces:%s:handler", space) ||
	  sscanf(prop, "script-space:%s:handler", space)) {
	 return query_script_space(space);
      }
      switch(prop) {
      case "script-spaces":
	 return map_indices(query_script_space_mapping());
      case "tick-usage":
	 return query_tick_usage();
      }
   }
}



int query_method(string method) {
   switch(lower_case(method)) {
   case "register_script_space":
   case "unregister_script_space":
      return TRUE;
   }
   return FALSE;
}

mixed
call_method(string method, mapping args) {
   switch(lower_case(method)) {
   case "register_script_space":
      if (typeof(args["space"]) != T_STRING) {
	 error("register_script_space expects $space");
      }
      if (typeof(args["handler"]) != T_OBJECT) {
	 error("register_script_space expects $handler");
      }
      register_script_space(args["space"], args["handler"]);
      break;
   case "unregister_script_space":
      if (typeof(args["space"]) != T_STRING) {
	 error("unregister_script_space expects $space");
      }
      unregister_script_space(args["space"]);
      break;
   }
}

int *query_node_map_stats()
{
   return ({ map_sizeof(node_map), 4 * NODE_COUNT, cleanup_stamp });
}
