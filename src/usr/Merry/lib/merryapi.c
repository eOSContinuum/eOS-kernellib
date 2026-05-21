/*
 * Merry invocation API.
 *
 * Lifted from SkotOS /usr/SkotOS/lib/merryapi.c per LM-2 sub-decisions.
 * Conventions adjusted to eos-kernellib:
 *   inherit "/lib/string"        -> inherit "/lib/util/ascii"
 *   inherit "/base/lib/urcalls"  -> inherit "/lib/util/lpc" (name(ob))
 *   "~SkotOS/sys/profiler" call  -> dropped (no profiler lifted)
 *   categorize_merry_word        -> game-content merryfun tokens removed
 *                                   (Act, Describe, EmitIn, EmitTo, SAM)
 */

# include <status.h>
# include <type.h>

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";


static
object find_merry(object ob, string signal, string mode) {
   string dprop, iprop;

   signal = lower_case(signal);
   mode = lower_case(mode);

   dprop = "merry:" + mode + ":" + signal;
   iprop = "merry:inherit:" + mode + ":" + signal;

   while (ob) {
      mixed code, new_val;

      code = ob->query_raw_property(dprop);
      if (typeof(code) == T_OBJECT) {
	 return code;
      }
      code = nil;
      new_val = ob->query_raw_property(iprop);
      while (typeof(new_val) == T_OBJECT) {
	 code = new_val;
	 new_val = code->query_raw_property(iprop);
      }
      if (typeof(code) == T_OBJECT) {
	 new_val = code->query_raw_property(dprop);
	 if (typeof(new_val) == T_OBJECT) {
	    return new_val;
	 }
      }
      ob = ob->query_ur_object();
   }
   return nil;
}

/*
 * This function is used by code that wants to deal with the
 * location of a specific merry script, rather than the script
 * itself.
 */

static
object find_merry_location(object ob, string signal, string mode) {
   string dprop, iprop;

   signal = lower_case(signal);
   mode = lower_case(mode);

   dprop = "merry:" + mode + ":" + signal;
   iprop = "merry:inherit:" + mode + ":" + signal;

   while (ob) {
      mixed code, new_val;

      code = ob->query_raw_property(dprop);
      if (typeof(code) == T_OBJECT) {
	 return ob;
      }
      code = nil;
      new_val = ob->query_raw_property(iprop);
      while (typeof(new_val) == T_OBJECT) {
	 code = new_val;
	 new_val = code->query_raw_property(iprop);
      }
      if (typeof(code) == T_OBJECT) {
	 new_val = code->query_raw_property(dprop);
	 if (typeof(new_val) == T_OBJECT) {
	    return code;
	 }
      }
      ob = ob->query_ur_object();
   }
   return nil;
}

static
mixed run_merry(object ob, string signal, string mode, mapping args,
		varargs string label) {
   object code;

   signal = lower_case(signal);
   mode = lower_case(mode);

   if (code = find_merry(ob, signal, mode)) {
      return code->evaluate(ob, signal, mode, args, label, name(ob) + "/" + mode + ":" + signal);
   }
   return TRUE;
}


static
mapping find_merries(object ob, string signal, string mode) {
   mapping imap;
   string prop, *ix;
   string dprop, iprop;
   mixed code, new_val;
   object *ancestry;
   mapping out;
   int i, j;

   signal = lower_case(signal);
   mode = lower_case(mode);

   dprop = "merry:" + mode + ":" + signal;
   iprop = "merry:inherit:" + mode + ":" + signal;

   out = ([ ]);

   ancestry = ({ });
   for (new_val = ob; new_val; new_val = new_val->query_ur_object()) {
      ancestry = ({ new_val }) + ancestry;
   }

   for (i = 0; i < sizeof(ancestry); i ++) {
      mixed val;

      ob = ancestry[i];

      val = ob->query_raw_property(dprop);
      if (typeof(val) == T_OBJECT) {
	 out[dprop] = val;
      }
      out += ob->query_prefixed_properties(dprop + "%");

      imap = ob->query_prefixed_properties(iprop + "%");
      imap[iprop] = ob->query_raw_property(iprop);

      ix = map_indices(imap);

      for (j = 0; j < sizeof(ix); j ++) {
	 code = nil;
	 new_val = imap[ix[j]];
	 while (typeof(new_val) == T_OBJECT) {
	    code = new_val;
	    new_val = code->query_raw_property(ix[j]);
	 }
	 if (typeof(code) == T_OBJECT) {
	    /* turn merry:inherit:foo into merry:foo */
	    prop = "merry:" + ix[j][14 ..];
	    new_val = code->query_raw_property(prop);
	    if (typeof(new_val) == T_OBJECT) {
	       out[prop] = new_val;
	    }
	 }
      }
   }
   return out;
}

static
mixed run_merries(object ob, string signal, string mode, mapping args,
		  varargs string label) {
   object *codes;
   int ret, i, sz;
   string *scripts;
   mapping map;

   signal = lower_case(signal);
   mode = lower_case(mode);

   map = find_merries(ob, signal, mode);
   sz = map_sizeof(map);
   codes = map_values(map);
   scripts = map_indices(map);

   ret = TRUE;
   for (i = 0; i < sz; i ++) {
      sscanf(scripts[i], "merry:%s:%s", mode, signal);
      ret &= !!codes[i]->evaluate(ob, signal, mode, args, label,
				  name(ob) + "/" + mode + ":" + signal);
   }
   return ret;
}


static
string categorize_merry_word(string word) {
   switch(lower_case(word)) {
   case "float":
   case "int":
   case "mapping":
   case "mixed":
   case "object":
   case "string":
   case "void":
      return "type";
   case "atomic":
   case "break":
   case "case":
   case "catch":
   case "continue":
   case "default":
   case "do":
   case "else":
   case "for":
   case "if":
   case "inherit":
   case "nil":
   case "nomask":
   case "private":
   case "return":
   case "rlimits":
   case "static":
   case "switch":
   case "varargs":
   case "while":
      return "keyword";
   case "Call":
   case "Duplicate":
   case "Error":
   case "Every":
   case "FindMerry":
   case "Get":
   case "GetVar":
   case "In":
   case "LabelCall":
   case "LabelRef":
   case "Set":
   case "SetVar":
   case "Slay":
   case "Spawn":
   case "Stop":
      return "merryfun";
   case "acos":
   case "allocate":
   case "allocate_float":
   case "allocate_int":
   case "asin":
   case "atan":
   case "atan2":
   case "call_trace":
   case "ceil":
   case "cos":
   case "cosh":
   case "crypt":
   case "ctime":
   case "error":
   case "exp":
   case "explode":
   case "fabs":
   case "find_object":
   case "floor":
   case "fmod":
   case "frexp":
   case "function_object":
   case "implode":
   case "ldexp":
   case "log":
   case "log10":
   case "map_indices":
   case "map_sizeof":
   case "map_values":
   case "millitime":
   case "modf":
   case "object_name":
   case "parse_string":
   case "pow":
   case "previous_object":
   case "previous_program":
   case "random":
   case "sin":
   case "sinh":
   case "sizeof":
   case "sqrt":
   case "sscanf":
   case "status":
   case "strlen":
   case "tan":
   case "tanh":
   case "this_object":
   case "time":
   case "typeof":
      return "kfun";
   }
   return nil;
}
