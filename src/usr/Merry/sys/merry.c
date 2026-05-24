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
 */
private
void _check_registrar(string target_name) {
   string caller_program, caller_domain, target_domain;

   caller_program = previous_program();
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
         " not authorized to register on " + target_name);
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
   string prop_name, low_timing;
   mixed existing;
   string *list;

   if (!ob) {
      error("register_observer: nil host object");
   }
   _check_registrar(::object_name(ob));

   low_timing = timing ? lower_case(timing) : "main";
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

void register_script_space(string space, object ob) {
   space = lower_case(space);
   if (space == "merry") {
      error("cannot register the merry script space");
   }
   _check_registrar(::object_name(ob));
   if (!script_spaces) {
      script_spaces = ([ ]);
   }
   script_spaces[space] = ob;
}

void unregister_script_space(string space) {
   object existing;

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
   _check_registrar(::object_name(existing));
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
