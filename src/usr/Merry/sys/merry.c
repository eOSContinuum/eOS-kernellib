/*
 * SYS_MERRY: the Merry daemon.
 *
 * Maintains a cache registry of compiled LPC snippets in the form of
 * /merry/<md5> objects. Hosts the yacc parser (via parse::create from
 * /lib/util/fileparse + grammar/merry.y) and exposes a runtime-extensible
 * script-space registry (register_script_space / space::method() syntax).
 *
 * Lifted from SkotOS /usr/SkotOS/sys/merry.c per LM-2 sub-decisions.
 * Stripped per LM-2 sub-decision (g):
 *   - signal-dispatch convenience methods (run_signal, do_new_signal,
 *     pre_signal, new_signal, desc_signal, post_signal, special_signal,
 *     post_hook) -- DD phase defines its own pre/main/post timing;
 *   - SAM admin endpoints (eval_sam_ref, patch_sam, patch_parser,
 *     patch_runner, patch_ticks);
 *   - clear_merry_cache (SkotOS file-cache cleanup; eos-kernellib uses
 *     a different runtime sync model per CT-2);
 *   - 'udat' pre-registered script-space (game-content);
 *   - SAMD->register_root + BASE_INITD->set_runner (game-content infra).
 *
 * Conventions adjusted to eos-kernellib:
 *   /lib/string  -> /lib/util/ascii
 *   /lib/fileparse -> /lib/util/fileparse
 *   SYS_MERRY    -> MERRY (eos-kernellib short-name convention)
 *   SysLog       -> sysLog
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
 * DD-1 (e) capability gate state. `approved_registrars` is the daemon-wide
 * allowlist of caller domains that may register observers / script-spaces
 * on objects outside their own domain; KERNEL programs always pass and
 * domain-match is the default allow path. Seeded with "System" +
 * "admin_console" at create(); extended via add_approved_registrar /
 * remove_approved_registrar (KERNEL-gated). Per-domain rather than
 * per-program — matches the cloud-server /usr/<domain>/ ownership model.
 *
 * `observer_cache` caches resolved observer lists per DI-1 (g). Broad
 * invalidation (clear the whole cache on any registration write) is the
 * MVA strategy; descendant-chain tracking is deferred per the DI-1 (g)
 * "implementation choice" framing.
 */
mapping approved_registrars;
mapping observer_cache;

/*
 * DI-2 batching surface state. `next_batch_id` is the daemon-wide
 * monotonically-increasing counter advanced at every batch boundary
 * (explicit and implicit) per DD-3 (b); statedump-persistent so batch
 * identity survives restart. `batch_status` is a DI-2 stub for the
 * batch-status entries DD-3 (c) calls for; keyed by batch-id, value is
 * ({ status, reason }). DI-5 will subsume this into the full change-log
 * surface (`query_changes_since`, retention pruning, etc.); the daemon-
 * local mapping here is enough for DI-2 smoke verification and for the
 * DI-3 dispatcher hooks to record outcomes against.
 */
int     next_batch_id;
mapping batch_status;

/*
 * TLS slot for the per-execution batch-context stack. Each entry is a
 * mapping { batch_id, atomic, opts, seq, cascade_depth } describing
 * one active batch frame; nested batches push/pop. Stack lifetime is
 * tied to the outermost call_limited frame (TLS reaps when that frame
 * returns), which gives "no batch active" the natural nil reading on
 * a fresh execution. Per-execution cycle-chain storage (DD-2 (b)
 * amendment) is a separate TLS slot DI-3 will manage.
 */
# define TLS_BATCH_STACK	"merry_batch_stack"

/* merry code begins 5 lines into the generated LPC file */
int query_line_offset() { return 5; }

string query_state_root() { return "Merry"; }

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

   node_map = ([ ]);
   compile_object("/usr/Merry/data/merry");
   compile_object("/usr/Merry/data/mcontext");

   set_object_name("Merry");

   /* /usr/Merry/{tmp,merry,merry/cleaned} are lazy-write targets.
    * /tmp/merry holds the yacc-generated parser scratch object;
    * /merry/<md5>.c holds the compiled wrapper objects produced by
    * data/merry::create(); /merry/cleaned/ catches files renamed
    * by merrynode::do_suicide during LRU eviction. None exist in
    * a fresh runtime tree, so create them here -- catch the EEXIST
    * shape so warm restarts don't error. */
   catch(make_dir("/usr/Merry/tmp"));
   catch(make_dir("/usr/Merry/merry"));
   catch(make_dir("/usr/Merry/merry/cleaned"));

   parse :: create("/usr/Merry/tmp/merry", "/usr/Merry/grammar/merry.y");
}

/*
 * _check_registrar: DD-1 (e) capability gate. Called by register_script_space,
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
 * _invalidate_observer_cache: broad invalidation per DI-1 (g) MVA choice.
 * Any observer-property write clears the whole cache. Args carried for
 * future descendant-chain-tracking switch; currently unused.
 */
private
void _invalidate_observer_cache(object ob, string path, string timing) {
   observer_cache = ([ ]);
}

/*
 * register_observer: DD-1 (d) procedural sugar for declarative observer
 * writes. Reads the existing list (per DI-6 (b) normalizes string and
 * array forms), appends new source, writes back via host's set_property.
 * Capability-gated per DD-1 (e); per-timing independent per DD-5 (b).
 * Writes the explicit form `merry:on:<path>:<timing>`; the alias form
 * `merry:on:<path>` (timing-less = implicit main) is accepted on read
 * by find_observers per DI-1 (b) MVP choice.
 */
void register_observer(object ob, string path, string timing, string source) {
   string caller_program, prop_name, low_timing;
   mixed existing;
   string *list;

   caller_program = previous_program();
   if (!ob) {
      error("register_observer: nil host object");
   }
   _check_registrar(caller_program, ::object_name(ob));

   low_timing = timing ? lower_case(timing) : "main";
   if (low_timing != "pre" && low_timing != "main" && low_timing != "post") {
      error("register_observer: timing must be one of {pre, main, post}; got \"" +
            low_timing + "\"");
   }
   prop_name = "merry:on:" + path + ":" + low_timing;

   existing = ob->query_raw_property(prop_name);
   if (typeof(existing) == T_ARRAY) {
      list = existing;
   } else if (typeof(existing) == T_STRING) {
      list = ({ existing });
   } else {
      list = ({ });
   }
   list += ({ source });
   ob->set_property(prop_name, list);

   _invalidate_observer_cache(ob, path, low_timing);
}

/*
 * Capability-set extension trio per DD-1 (e) amendment 2026-05-22. add and
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
 * DI-2 batching surface. Implements DD-1 (c) hybrid batching with the
 * DD-4 (d) atomic-mode opt-in. Two public LFUNs (`batch`, `batched_set`)
 * and a small set of internal helpers exposed for DI-3 (dispatcher) and
 * DI-5 (change-log) to weave into. Per DD-1 (e) Amendment 2026-05-22 the
 * capability gate is LFUN-only at registration entry points -- batching
 * writes through `set_property` and is not gated here.
 *
 * Per-execution context lives in TLS under TLS_BATCH_STACK as a stack of
 * { batch_id, atomic, opts, seq, cascade_depth } mappings. Nested batches
 * push; exit pops. The DD-2 (b) amendment puts the cycle-chain at a
 * separate TLS slot (DI-3 owns); the batch-stack here tracks only what
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
 * Records the batch-status entry per DD-3 (c). DI-2 stub: writes to the
 * daemon-local `batch_status` mapping. DI-5 will replace this with the
 * full change-log surface (`query_changes_since` + retention + per-host
 * pruning). The seven-status enum lands here -- DI-2 only emits
 * "completed" and "main-aborted" since pre/main/post timing dispatch and
 * cascade-depth / cycle-detection live in DI-3.
 */
private
void _record_batch_status(int batch_id, string status, mixed reason) {
   batch_status[batch_id] = ({ status, reason });
}

/*
 * Public read-only accessors over the batch-stack TLS slot. DI-3 calls
 * _current_batch_id() to decide whether to allocate an implicit batch on
 * an unbatched property write; DI-3 + DI-5 read _current_batch_context()
 * to track cascade-depth and to fetch the active batch-id when writing
 * change-log tuples; DI-5 calls _current_batch_seq_advance() per tuple.
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
 * DI-2 hooks for the DI-3 implicit-single-mutation path per
 * sub-deliverable (e) and DD-3 (b). DI-3's dispatcher calls
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
 * (per DD-4 (d) opt-in semantics). The atomic-mode status-entry write
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
       * Per DD-3 (b) seq increments per write under the active batch-id.
       * DI-5 will read this seq when writing tuples; DI-3 will fire
       * pre/main/post observers around the set_property call. For DI-2
       * scope, just advance the counter and perform the property write.
       */
      _current_batch_seq_advance();
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
 * batch: DD-1 (c) function-reference batching API. Invokes
 * obj->func(args...) inside a fresh batch context; atomic-mode opt-in
 * via opts mapping per DD-4 (d) (`([ "atomic": 1 ])`). Non-atomic is
 * the DD-2 (d) catch'd-error default: on throw, a "main-aborted" status
 * entry persists per DD-3 (c) and the error propagates to the caller's
 * catch{}. In atomic-mode, DGD rolls back both mutations and the status
 * entry (the atomic batch leaves no trace per DD-4 (d)).
 *
 * Not callable from Merry observer source per DD-1 (c) -- the SANDBOX
 * whitelist in merrynode.c does not expose this verb because L14 #15
 * forbids function-reference syntax in Merry.
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
 * batched_set: DD-1 (c) mapping-write batching API. Writes the kv_map's
 * key/value pairs to obj via set_property; all writes share a single
 * batch-id with sequential seq values starting at 0 (per DD-3 (b)).
 * Atomic-mode opt-in identical to batch() per DD-4 (d).
 *
 * Callable from Merry observer source (via the BatchedSet merryfun in
 * merrynode.c) because the mapping-arg signature satisfies L14 #15
 * (no function-reference required; arguments compose inline).
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
