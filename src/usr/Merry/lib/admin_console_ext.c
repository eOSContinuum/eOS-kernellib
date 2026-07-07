/*
 * Merry's admin_console extension. Provides operator-tier verbs for
 * inspecting and mutating dispatcher state from the kernel admin
 * console.
 *
 * A selective extension: the verbs are registered in
 * ADMIN_CONSOLE_REGISTRY's hardcoded dispatch_table; admin_console's
 * switch-default branch routes unknown verbs through that registry,
 * find_object's the clone of this library at OBJ_MERRY_ADMIN_CONSOLE_EXT,
 * and call_other's the matching cmd_<verb> method here.
 *
 * Verb-set (9 verbs):
 *
 *   READ / DIAGNOSTIC (4):
 *     observers <obj_path> [<path> [timing]] [-effective]
 *     cascade-depth [N]
 *     batch-status <batch_id>
 *     dispatch-trace on|off|status
 *
 *   MUTATION (5):
 *     register-observer <obj_path> <path> <timing> <source...>
 *     unregister-observer <obj_path> <path> <timing> [index]
 *     query-approved-registrars
 *     approve-registrar <domain>
 *     unapprove-registrar <domain>
 *
 * Methods that mutate state route through ADMIN_CONSOLE_REGISTRY's
 * verb_* elevation helpers; the registry is /kernel/-tier so KERNEL()
 * and _check_registrar gates on the underlying MERRY LFUNs pass. The
 * registry's _check_caller gate constrains who can use the elevation
 * surface (only this library's program path is in allowed_callers).
 * The narrow elevation surface here is a mini-capability-model; a
 * future capability-model layer can generalize it across all
 * kernel-layer subsystems.
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
# include <log.h>

# define INDEX	"/usr/Index/sys/index_daemon"

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
 * _resolve_target: turn a verb's object argument into an object. LPC
 * path first (find_object), then the Index logical-name registry --
 * the same resolution order the coercion codec uses for object
 * references. Logical names are the sanctioned address for clones,
 * which the System auto layer's find_object deliberately refuses in
 * raw path#index form. Emits the unified not-found diagnostic and
 * returns nil when neither route resolves.
 */
private object _resolve_target(object user, string verb, string target_path) {
   object target;

   target = find_object(target_path);
   if (!target) {
      target = INDEX->query_object(target_path);
   }
   if (!target) {
      _emit(user, verb + ": target not found (no loaded object or Index name): " +
            target_path + "\n");
   }
   return target;
}

/*
 * observers <obj_path> [<path> [timing]] [-effective]
 *
 * All three read shapes route through MERRY's public query LFUNs,
 * which read the target's property table directly (never the daemon
 * cache -- ground truth for a diagnostic verb):
 *
 *   no <path>       enumerate the object's observed (path, timing)
 *                   slots (query_observed_paths) -- the discovery step
 *                   for an operator who does not know the paths.
 *   <path> [timing] the local slot(s), indexed (query_observers); the
 *                   indices are what unregister-observer's optional
 *                   index argument removes by.
 *   -effective      the ancestry-walk view (query_effective_observers):
 *                   what the dispatcher would fire, each entry labeled
 *                   with the owning ancestor. Requires a <path>.
 */
void cmd_observers(object user, string cmd, string str) {
   string *raw, *parts;
   string target_path, prop_path, timing;
   string *timings;
   object target;
   int i, effective;

   raw = str ? (explode(str, " ") - ({ "" })) : ({ });
   parts = ({ });
   effective = 0;
   for (i = 0; i < sizeof(raw); i ++) {
      if (raw[i] == "-effective") {
         effective = 1;
      } else {
         parts += ({ raw[i] });
      }
   }
   if (sizeof(parts) == 0) {
      _emit(user, "usage: observers <obj_path> [<path> [timing]] [-effective]\n");
      return;
   }
   target_path = parts[0];

   target = _resolve_target(user, "observers", target_path);
   if (!target) {
      return;
   }

   if (sizeof(parts) == 1) {
      mixed **pairs;

      if (effective) {
         _emit(user, "observers: -effective requires a <path>\n");
         return;
      }
      pairs = MERRY->query_observed_paths(target);
      _emit(user, target_path + " observed paths:\n");
      if (sizeof(pairs) == 0) {
         _emit(user, "  (none)\n");
         return;
      }
      for (i = 0; i < sizeof(pairs); i ++) {
         _emit(user, "  " + pairs[i][0] + " : " + pairs[i][1] + "\n");
      }
      return;
   }

   prop_path = parts[1];
   timing = sizeof(parts) >= 3 ? lower_case(parts[2]) : nil;

   if (timing &&
       timing != "pre" && timing != "main" && timing != "post") {
      _emit(user, "observers: timing must be one of {pre, main, post}\n");
      return;
   }

   timings = timing ? ({ timing }) : ({ "pre", "main", "post" });
   _emit(user, target_path + " " + prop_path +
         (effective ? " (effective)" : "") + ":\n");
   for (i = 0; i < sizeof(timings); i ++) {
      _emit(user, "  " + timings[i] + ":\n");
      if (effective) {
         mixed **entries;
         int j;

         entries = MERRY->query_effective_observers(target, prop_path,
                                                    timings[i]);
         if (sizeof(entries) == 0) {
            _emit(user, "    (none)\n");
            continue;
         }
         for (j = 0; j < sizeof(entries); j ++) {
            _emit(user, "    [" + (string) j + "] " + entries[j][0] +
                  " " + entries[j][1] + "\n");
         }
      } else {
         string *descs;
         int j;

         descs = MERRY->query_observers(target, prop_path, timings[i]);
         if (sizeof(descs) == 0) {
            _emit(user, "    (none)\n");
            continue;
         }
         for (j = 0; j < sizeof(descs); j ++) {
            _emit(user, "    [" + (string) j + "] " + descs[j] + "\n");
         }
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
 * state. When on, _trace_dispatch in MERRY emits entry events to the
 * general logd stream at DEBUG level; when off (the default), trace
 * sites elide their I/O. Turning trace on while logd's threshold is
 * above DEBUG earns a hint, since the lines would be silently dropped.
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
   if (arg == "on") {
      object logd;

      if ((logd=find_object(LOGD)) && logd->query_threshold() > LOG_DEBUG) {
         _emit(user, "note: trace lines emit at DEBUG and the current " +
               "log-level suppresses them; `log-level debug` to see them\n");
      }
   }
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

   target = _resolve_target(user, "register-observer", target_path);
   if (!target) {
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
 * unregister-observer <obj_path> <path> <timing> [index]
 *
 * Without index: removes all observers at (target, path, timing) --
 * coarse granularity (the daemon's unregister_observer). With index:
 * removes the single slot entry at that position (the daemon's
 * remove_observer; indices as shown by `observers <obj_path> <path>`).
 */
void cmd_unregister_observer(object user, string cmd, string str) {
   string *parts;
   string target_path, prop_path, timing;
   object target;
   mixed err;

   parts = _split(str, 4, 3);
   if (!parts || sizeof(parts) > 4) {
      _emit(user,
            "usage: unregister-observer <obj_path> <path> <timing> [index]\n");
      return;
   }
   target_path = parts[0];
   prop_path = parts[1];
   timing = parts[2];

   target = _resolve_target(user, "unregister-observer", target_path);
   if (!target) {
      return;
   }

   if (sizeof(parts) == 4) {
      int index;

      if (sscanf(parts[3], "%d", index) != 1) {
         _emit(user, "unregister-observer: index must be an integer\n");
         return;
      }
      err = catch(ADMIN_CONSOLE_REGISTRY->
                  verb_remove_observer(target, prop_path, timing, index));
      if (err) {
         _emit(user, "unregister-observer: " + err + "\n");
         return;
      }
      _emit(user, "unregister-observer: removed [" + (string) index +
            "] from " + target_path + " " + prop_path + ":" + timing + "\n");
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
 * Routes through the registry helper, which grants the "merry.registrar"
 * capability against capabilityd (KERNEL-gated).
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
 * Routes through the registry helper, which revokes the "merry.registrar"
 * capability against capabilityd (KERNEL-gated).
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
