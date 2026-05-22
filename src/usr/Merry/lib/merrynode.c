/*
 * Per-script base class for compiled Merry programs.
 *
 * Provides:
 *   - the sandbox (37 forbidden kfuns/methods + call_other and new_object
 *     restrictions; '->' and 'rlimits' additionally grammar-forbidden);
 *   - the merryfun API surface callable from Merry source: property bridge
 *     (Set/Get/SetVar/GetVar), invocation (Call/LabelCall/LabelRef/FindMerry),
 *     lifecycle (Spawn/Slay/Duplicate), scheduling (In/Every/Stop);
 *   - the args + this TLS-shape state set up at evaluate() entry;
 *   - the obref() runtime lookup that resolves AST `${objref-name}` tokens
 *     to actual objects via the per-script object array.
 *
 * Lifted from SkotOS /usr/SkotOS/lib/merrynode.c per LM-2 sub-decisions.
 * Game-content merryfuns dropped: Social / Bilbo / Popup / Act / Describe
 * / Match / MatchPlural / EmitTo / EmitIn / UnSAM / ParseXML and their
 * SkotOS-only inherit chain (bilbo, ursocials, describe, xmd).
 * SAM surface dropped: samref(), samarr, set_sam_array().
 * Spawn inlined as clone_object + set_ur_object (SkotOS spawn_thing was
 * game-content "thing" predicated).
 *
 * /lib/womble inherit + womble_merry() callback dropped at LM-4 (was the
 * last surviving SAM-cleanup hook; never invoked under cloud-server -- the
 * SkotOS caller was data/merry's womble_merry which itself was removed at
 * LM-3 when samarr was dropped per LM-2 sub-decision (c)).
 */

# include <type.h>

# define MERRY		"/usr/Merry/sys/merry"

/* string/util surface available to Merry source */
inherit "/lib/util/ascii";

/* name(ob) and other LPC primitives */
inherit "/lib/util/lpc";

/* invocation API */
private inherit "/usr/Merry/lib/merryapi";

/* state export/import for Duplicate */
private inherit "/usr/Marshal/XmlBinding/lib/stateimpex";

private int mru_stamp;
private object *obarr;

object this;	/* make this a TLS one day? */
mapping args;	/* make this a TLS one day? */

string stored_description;

mixed evaluate(object t, string signal, string mode, mapping a, string label,
	       varargs string description) {
   mapping oldargs;
   object oldthis;
   mixed res, oat;

   mru_stamp = time();

   oldthis = this;
   this = t;

   oldargs = args;
   args = a;

   oat = args["this"];
   args["this"] = this;

   stored_description = description;

   args["this"] = this;

   res = ::call_other(::this_object(), "merry", signal,
		      mode, label ? label : "virgin");

   stored_description = nil;

   args["this"] = oat;

   args = oldargs;
   this = oldthis;

   return res;
}

string query_description() {
   return stored_description;
}

int query_mru_stamp() {
   return mru_stamp;
}

void set_object_array(object *arr) {
   obarr = arr;
}

static
object obref(int i) {
   return obarr[i];
}


object *query_obarr() { return obarr; }	/* for debugging */

nomask
void suicide() {
   :: call_out("do_suicide", 0);
}

static
void do_suicide() {
   catch {
      string path;

      path = object_name(this_object());
      if (file_info(path + ".c")) {
	 string name;

	 sscanf(path, "/usr/Merry/merry/%s", name);
	 if (file_info("/usr/Merry/merry/cleaned/" + name + ".c")) {
	    remove_file("/usr/Merry/merry/cleaned/" + name + ".c");
	 }
	 rename_file(path + ".c", "/usr/Merry/merry/cleaned/" + name + ".c");
      }
   }
   /* cloud-server destruct_object takes an explicit object arg; SkotOS
    * had a no-arg shorthand that destructed this_object(). */
   :: destruct_object(this_object());
}

static
void do_delay(string mode, string signal, mixed delay, string label) {
   if (!this || !signal || !mode) {
      error("this merry node cannot perform delays");
   }
   :: call_other(this, "delayed_call",
		 ::new_object("/usr/Merry/data/mcontext",
			      signal,
			      mode,
			      label,
			      args + ([ ])),
		 "merry_continuation",
		 delay, this);
}

private atomic
void set_all(object o, mapping m) {
   string *ix;
   int i;

   :: call_other(o, "clear_all_properties");

   ix = map_indices(m);
   for (i = 0; i < sizeof(ix); i ++) {
      :: call_other(o, "set_property", ix[i], m[ix[i]]);
   }
}


nomask static
mixed Set(object o, string p, mixed v) {
   if (!o) {
      error("Missing object for Set of " + dumpValue(p));
   }
   if (p == "*") {
      set_all(o, v);
      return nil;
   }
   return ::call_other(o, "set_property", p, v);
}

nomask static
mixed Get(object o, string p) {
   if (!o) {
      error("Missing object for Get of " + dumpValue(p));
   }
   if (p == "*") {
      return ::call_other(o, "query_properties");
   }
   return ::call_other(o, "query_property", p);
}

nomask static
mixed SetVar(string n, mixed v) {
   if (n) {
      args[lower_case(n)] = v;
   }
}

nomask static
mixed GetVar(string n) {
   if (n) {
      return args[lower_case(n)];
   }
}

nomask static
object FindMerry(mixed oref, string mode, string signal) {
   object obj;

   if (typeof(oref) == T_OBJECT) {
      obj = oref;
   } else if (typeof(oref) == T_STRING) {
      obj = ::find_object(oref);
      if (!obj) {
	 error("cannot find object: " + oref);
      }
   } else {
      error("bad first argument to FindMerry()");
   }
   return find_merry_location(obj, signal, mode);
}

nomask static
mixed Call(mixed oref, string name, varargs mixed *local) {
   string *save_ix;
   object obj;
   mixed *save_val;
   mixed res;
   int i, method;

   if (!local) {
      local = ({ });
   }
   if (sizeof(local) % 2) {
      error("uneven number of parameters to Call()");
   }
   if (typeof(oref) == T_OBJECT) {
      obj = oref;
   } else if (typeof(oref) == T_STRING) {
      obj = ::find_object(oref);
      if (!obj) {
	 error("cannot find object: " + oref);
      }
   } else {
      error("bad first argument to Call()");
   }

   method = !!obj->query_method(name);
   if (!method) {
      if (!find_merry(obj, name, "lib")) {
	 error("cannot find script [" + name + "] on " + name(obj));
      }
   }

   save_ix  = allocate(sizeof(local)/2);
   save_val = allocate(sizeof(local)/2);

   for (i = 0; i < sizeof(save_ix); i ++) {
      save_ix[i] = lower_case(local[i*2]);
      save_val[i] = args[save_ix[i]];
      args[save_ix[i]] = local[i*2+1];
   }

   if (method) {
      res = obj->call_method(name, args);
   } else {
      res = run_merry(obj, name, "lib", args, nil);
   }

   for (i = 0; i < sizeof(save_ix); i ++) {
      args[save_ix[i]] = save_val[i];
   }
   return res;
}

nomask static
mixed LabelCall(string space, string fun, varargs mixed *local) {
   object handler;

   if (handler = MERRY->query_script_space(space)) {
      return Call(handler, fun, local);
   }
   error("cannot find script space: " + space);
}

nomask static
object LabelRef(string space) {
   return MERRY->query_script_space(space);
}

nomask static atomic
object Spawn(object ur) {
   string clonable;
   object newobj;

   if (typeof(ur) != T_OBJECT) {
      error("argument 1 to Spawn() is not an object");
   }
   if (sscanf(object_name(ur), "%s#", clonable)) {
      /* :: escape past the local SANDBOX(clone_object) shadow.
       * Same idiom as the ::call_other / ::destruct_object / ::new_object
       * escapes elsewhere in this file. */
      newobj = ::clone_object(clonable);
      newobj->set_ur_object(ur);
      return newobj;
   }
   error("argument 1 to Spawn() must be a clone or clonable");
}

nomask static
void Slay(object obj) {
   if (!obj) {
      error("Missing object for Slay");
   }
   :: call_other(obj, "suicide");
}

nomask static
string In(string signal, int seconds) {
   if (!this) {
      error("this merry node cannot perform delays");
   }
   return ::call_other(this, "schedule_entry",
		       signal, time() + seconds);
}

nomask static
string Every(string signal, int seconds) {
   if (!this) {
      error("this merry node cannot perform delays");
   }
   return ::call_other(this, "schedule_entry",
		       signal, time() + seconds, seconds);
}

nomask static
string Stop(string id) {
   if (!this) {
      error("this merry node cannot perform delays");
   }
   return ::call_other(this, "unschedule_entry", id);
}


nomask static atomic
object Duplicate(object ob) {
   string clonable;
   object clone;
   mixed state;

   if (!ob) {
      error("Missing object for Duplicate");
   }
   if (sscanf(object_name(ob), "%s#%*d", clonable)) {
      state = export_state(ob);
      /* :: escape past the local SANDBOX(clone_object) shadow.
       * See Spawn() above. */
      clone = ::clone_object(clonable);
      import_state(clone, state);
      return clone;
   }
   error("object is not a clone");
}

static
mixed call_other(mixed obj, string fun, mixed args...) {
   switch(typeof(obj)) {
   case T_NIL:
   case T_INT:
   case T_FLOAT:
   case T_ARRAY:
   case T_MAPPING:
      return ::call_other(obj, fun, args...);
   case T_OBJECT:
      break;
   }
   error("function 'call_other' not allowed in merry code");
}

static
object new_object(string obj, mixed args...) {
   error("function 'new_object' not allowed in merry code");
}

# define SANDBOX(f) mixed f(mixed args...) { error("function '" + #f + "' not allowed in merry code"); }

/* SkotOS-era forbidden set (37 functions verbatim). Several names
 * may not resolve to actual DGD kfuns in cloud-server (e.g.
 * add_event, event, subscribe_event, ports, open_port, this_user,
 * users, set_object_name) -- the SANDBOX entries are harmless
 * because the kfun-not-present case can't be reached from Merry
 * source either. Kept for conservative-deny posture. */
SANDBOX(add_event)
SANDBOX(block_input)
SANDBOX(call_touch)
/* SANDBOX(call_out) */
SANDBOX(clone_object)
SANDBOX(compile_object)
SANDBOX(connect)
SANDBOX(destruct_object)
SANDBOX(destruct_program)
SANDBOX(dump_state)
SANDBOX(editor)
SANDBOX(event)
SANDBOX(event_except)
SANDBOX(execute_program)
SANDBOX(file_info)
SANDBOX(function_object)
SANDBOX(get_dir)
SANDBOX(make_dir)
SANDBOX(open_port)
SANDBOX(ports)
SANDBOX(query_editor)
SANDBOX(query_originator)
SANDBOX(read_file)
SANDBOX(remove_call_out)
SANDBOX(remove_dir)
SANDBOX(remove_event)
SANDBOX(remove_file)
SANDBOX(rename_file)
SANDBOX(send_datagram)
SANDBOX(send_message)
SANDBOX(set_object_name)
SANDBOX(set_originator)
SANDBOX(shutdown)
SANDBOX(subscribe_event)
SANDBOX(swapout)
SANDBOX(this_user)
SANDBOX(unsubscribe_event)
SANDBOX(users)
SANDBOX(write_file)

/* LM-3 cloud-server kfun-delta additions (per LM-2 sub-decision (b)
 * audit; kfuns present in DGD 1.7.x cloud-server but not in
 * SkotOS-era DGD or not in SkotOS's sandbox list). Conservative-deny:
 * each is either escape-shaped (state restore, program removal,
 * file dump) or external-effect-shaped (UDP/telnet networking,
 * connection disconnect). */
SANDBOX(remove_program)
SANDBOX(restore_object)
SANDBOX(dump_file)
SANDBOX(dump_interval)
SANDBOX(send_close)
SANDBOX(connect_datagram)
SANDBOX(datagram_attach)
SANDBOX(datagram_challenge)
SANDBOX(datagram_connect)
SANDBOX(datagram_port)
SANDBOX(datagram_users)
SANDBOX(telnet_connect)
SANDBOX(telnet_port)
