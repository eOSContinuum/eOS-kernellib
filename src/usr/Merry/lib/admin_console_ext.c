/*
 * Merry's admin_console extension. Provides operator-tier verbs for
 * inspecting and mutating dispatcher state from the kernel admin
 * console.
 *
 * #EX-3 selective extension (P3 layering). The verbs are registered in
 * ADMIN_CONSOLE_REGISTRY's hardcoded dispatch_table; admin_console's
 * switch-default branch routes unknown verbs through that registry,
 * find_object's the clone of this library at OBJ_MERRY_ADMIN_CONSOLE_EXT,
 * and call_other's the matching cmd_<verb> method here.
 *
 * Verb-set (9 verbs per #EX-3 sub-decision 2):
 *
 *   READ / DIAGNOSTIC (4):
 *     observers <obj_path> <path> [timing]
 *     cascade-depth [N]
 *     batch-status <batch_id>
 *     dispatch-trace on|off|status
 *
 *   MUTATION (5):
 *     register-observer <obj_path> <path> <timing> <source...>
 *     unregister-observer <obj_path> <path> <timing>
 *     query-approved-registrars
 *     approve-registrar <domain>
 *     unapprove-registrar <domain>
 *
 * Methods that mutate state route through ADMIN_CONSOLE_REGISTRY's
 * verb_* elevation helpers; the registry is /kernel/-tier so KERNEL()
 * and _check_registrar gates on the underlying MERRY LFUNs pass. The
 * registry's _check_caller gate constrains who can use the elevation
 * surface (only this library's program path is in allowed_callers).
 * The narrow elevation surface here is a mini-capability-model that
 * the Seed 11 future workstream generalizes across all kernel-layer
 * subsystems.
 *
 * Output: each verb writes to user->message() directly with the user
 * arg passed in by the dispatcher; admin_console's private message()
 * is not reachable from a /usr/-tier extension and the equivalence
 * holds because the kernel's message() forwards to user->message()
 * unchanged (see /kernel/obj/admin_console::message).
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <Merry.h>
# include <type.h>

private inherit "/lib/util/ascii";	/* lower_case */

/*
 * _emit: convenience wrapper. The dispatcher passes the user object as
 * arg 1 to every cmd_*; routing through user->message keeps the output
 * path identical to the inline kernel verbs.
 */
private void _emit(object user, string msg) {
   if (user) {
      user->message(msg);
   }
}

/*
 * _split: pull leading tokens off str up to `n_lead`, capturing the
 * rest as the final element. Returns nil if fewer than `n_required`
 * tokens are present. For verbs like register-observer where the final
 * argument (source) may contain spaces, n_lead < n_required cleanly
 * captures the trailing remainder.
 */
private string *_split(string str, int n_lead, int n_required) {
   string *parts;
   string *result;
   string rest;
   int i;

   if (!str) {
      str = "";
   }
   parts = explode(str, " ") - ({ "" });
   if (sizeof(parts) < n_required) {
      return nil;
   }
   if (sizeof(parts) <= n_lead) {
      return parts;
   }
   result = parts[.. n_lead - 1];
   rest = implode(parts[n_lead ..], " ");
   return result + ({ rest });
}

/*
 * observers <obj_path> <path> [timing]
 *
 * Reads merry:on:<path>:<timing> property on the target object directly
 * (bypasses the daemon cache, which is the right call for a diagnostic
 * verb -- shows ground truth, not cache state). Lists the compiled
 * observer object names per timing.
 */
void cmd_observers(object user, string cmd, string str) {
   string *parts;
   string target_path, prop_path, timing;
   string *timings;
   object target;
   int i;

   parts = _split(str, 3, 2);
   if (!parts) {
      _emit(user, "usage: observers <obj_path> <path> [timing]\n");
      return;
   }
   target_path = parts[0];
   prop_path = parts[1];
   timing = sizeof(parts) >= 3 ? lower_case(parts[2]) : nil;

   target = find_object(target_path);
   if (!target) {
      _emit(user, "observers: target object not loaded: " + target_path + "\n");
      return;
   }

   if (timing &&
       timing != "pre" && timing != "main" && timing != "post") {
      _emit(user, "observers: timing must be one of {pre, main, post}\n");
      return;
   }

   timings = timing ? ({ timing }) : ({ "pre", "main", "post" });
   _emit(user, target_path + " " + prop_path + ":\n");
   for (i = 0; i < sizeof(timings); i ++) {
      string prop_name;
      mixed val;

      prop_name = "merry:on:" + prop_path + ":" + timings[i];
      val = target->query_raw_property(prop_name);
      _emit(user, "  " + timings[i] + ":\n");
      if (val == nil) {
         _emit(user, "    (none)\n");
      } else if (typeof(val) == T_ARRAY) {
         int j;
         if (sizeof(val) == 0) {
            _emit(user, "    (empty)\n");
         }
         for (j = 0; j < sizeof(val); j ++) {
            mixed entry;
            entry = val[j];
            if (typeof(entry) == T_OBJECT) {
               _emit(user, "    " + object_name(entry) + "\n");
            } else if (typeof(entry) == T_STRING) {
               _emit(user, "    <source string>\n");
            } else {
               _emit(user, "    <unexpected type " +
                     (string) typeof(entry) + ">\n");
            }
         }
      } else if (typeof(val) == T_OBJECT) {
         _emit(user, "    " + object_name(val) + "\n");
      } else if (typeof(val) == T_STRING) {
         _emit(user, "    <source string>\n");
      } else {
         _emit(user, "    <unexpected type " +
               (string) typeof(val) + ">\n");
      }
   }
}

/*
 * cascade-depth [N]
 *
 * No arg: report current max_cascade_depth.
 * With N: route through registry helper (set is KERNEL-gated on MERRY).
 */
void cmd_cascade_depth(object user, string cmd, string str) {
   string *parts;
   int n;
   mixed err;

   parts = _split(str, 1, 0);
   if (parts == nil || sizeof(parts) == 0) {
      _emit(user, "cascade-depth: " +
            (string) MERRY->query_max_cascade_depth() + "\n");
      return;
   }
   if (sscanf(parts[0], "%d", n) != 1) {
      _emit(user, "cascade-depth: argument must be an integer\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->verb_set_max_cascade_depth(n));
   if (err) {
      _emit(user, "cascade-depth: " + err + "\n");
      return;
   }
   _emit(user, "cascade-depth set to " + (string) n + "\n");
}

/*
 * batch-status <batch_id>
 *
 * Calls MERRY->_query_batch_status; renders the (status, reason) tuple
 * or reports "no entry" for unknown batch ids.
 */
void cmd_batch_status(object user, string cmd, string str) {
   string *parts;
   int batch_id;
   mixed *entry;

   parts = _split(str, 1, 1);
   if (!parts) {
      _emit(user, "usage: batch-status <batch_id>\n");
      return;
   }
   if (sscanf(parts[0], "%d", batch_id) != 1) {
      _emit(user, "batch-status: batch_id must be an integer\n");
      return;
   }
   entry = MERRY->_query_batch_status(batch_id);
   if (!entry) {
      _emit(user, "batch-status " + (string) batch_id + ": no entry\n");
      return;
   }
   _emit(user, "batch-status " + (string) batch_id + ": " +
         entry[0] +
         (entry[1] ? " (" + (string) entry[1] + ")" : "") + "\n");
}

/*
 * dispatch-trace on|off|status
 *
 * Toggles dispatch_trace flag via registry helper, or reports current
 * state. When on, _trace_dispatch in MERRY appends entry events to
 * MERRY_LOG_FILE; when off (the default), trace sites elide their I/O.
 */
void cmd_dispatch_trace(object user, string cmd, string str) {
   string *parts;
   string arg;
   mixed err;

   parts = _split(str, 1, 1);
   if (!parts) {
      _emit(user, "usage: dispatch-trace on|off|status\n");
      return;
   }
   arg = lower_case(parts[0]);
   if (arg == "status") {
      _emit(user, "dispatch-trace: " +
            (MERRY->query_dispatch_trace() ? "on" : "off") + "\n");
      return;
   }
   if (arg != "on" && arg != "off") {
      _emit(user, "dispatch-trace: argument must be on|off|status\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->
               verb_set_dispatch_trace(arg == "on" ? 1 : 0));
   if (err) {
      _emit(user, "dispatch-trace: " + err + "\n");
      return;
   }
   _emit(user, "dispatch-trace " + arg + "\n");
}

/*
 * register-observer <obj_path> <path> <timing> <source...>
 *
 * Source captures the rest of the line (may contain spaces) per _split's
 * tail-capture mode. Routes through registry helper because the caller
 * domain (Merry) is not in approved_registrars by default and shouldn't
 * need to be just to enable this verb.
 */
void cmd_register_observer(object user, string cmd, string str) {
   string *parts;
   string target_path, prop_path, timing, source;
   object target;
   mixed err;

   parts = _split(str, 3, 4);
   if (!parts) {
      _emit(user, "usage: register-observer <obj_path> <path> <timing> <source...>\n");
      return;
   }
   target_path = parts[0];
   prop_path = parts[1];
   timing = parts[2];
   source = parts[3];

   target = find_object(target_path);
   if (!target) {
      _emit(user, "register-observer: target object not loaded: " +
            target_path + "\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->
               verb_register_observer(target, prop_path, timing, source));
   if (err) {
      _emit(user, "register-observer: " + err + "\n");
      return;
   }
   _emit(user, "register-observer: registered on " +
         target_path + " " + prop_path + ":" + timing + "\n");
}

/*
 * unregister-observer <obj_path> <path> <timing>
 *
 * Removes all observers at (target, path, timing). MVA-scope coarse
 * granularity (matches the daemon's unregister_observer signature).
 */
void cmd_unregister_observer(object user, string cmd, string str) {
   string *parts;
   string target_path, prop_path, timing;
   object target;
   mixed err;

   parts = _split(str, 3, 3);
   if (!parts) {
      _emit(user, "usage: unregister-observer <obj_path> <path> <timing>\n");
      return;
   }
   target_path = parts[0];
   prop_path = parts[1];
   timing = parts[2];

   target = find_object(target_path);
   if (!target) {
      _emit(user, "unregister-observer: target object not loaded: " +
            target_path + "\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->
               verb_unregister_observer(target, prop_path, timing));
   if (err) {
      _emit(user, "unregister-observer: " + err + "\n");
      return;
   }
   _emit(user, "unregister-observer: cleared " +
         target_path + " " + prop_path + ":" + timing + "\n");
}

/*
 * query-approved-registrars
 *
 * Read-only -- no gate, calls public MERRY->query_approved_registrars.
 */
void cmd_query_approved_registrars(object user, string cmd, string str) {
   string *domains;
   int i;

   domains = MERRY->query_approved_registrars();
   if (!domains || sizeof(domains) == 0) {
      _emit(user, "approved registrars: (none)\n");
      return;
   }
   _emit(user, "approved registrars:\n");
   for (i = 0; i < sizeof(domains); i ++) {
      _emit(user, "  " + domains[i] + "\n");
   }
}

/*
 * approve-registrar <domain>
 *
 * Routes through registry helper (underlying add_approved_registrar is
 * KERNEL-gated).
 */
void cmd_approve_registrar(object user, string cmd, string str) {
   string *parts;
   mixed err;

   parts = _split(str, 1, 1);
   if (!parts) {
      _emit(user, "usage: approve-registrar <domain>\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->verb_approve_registrar(parts[0]));
   if (err) {
      _emit(user, "approve-registrar: " + err + "\n");
      return;
   }
   _emit(user, "approve-registrar: " + parts[0] + " added\n");
}

/*
 * unapprove-registrar <domain>
 *
 * Routes through registry helper (underlying remove_approved_registrar
 * is KERNEL-gated).
 */
void cmd_unapprove_registrar(object user, string cmd, string str) {
   string *parts;
   mixed err;

   parts = _split(str, 1, 1);
   if (!parts) {
      _emit(user, "usage: unapprove-registrar <domain>\n");
      return;
   }
   err = catch(ADMIN_CONSOLE_REGISTRY->verb_unapprove_registrar(parts[0]));
   if (err) {
      _emit(user, "unapprove-registrar: " + err + "\n");
      return;
   }
   _emit(user, "unapprove-registrar: " + parts[0] + " removed\n");
}
