/*
 * Admin-console verb registry. KERNEL-tier singleton consulted by
 * /kernel/obj/admin_console::process() in the switch-default branch
 * when an incoming verb is not in the hardcoded built-in set.
 *
 * A selective extension surface. The registry holds the dispatch_table
 * (verb -> ([ "path": <ext_obj_path>, "method": <method_name> ])),
 * consulted by admin_console::process() at unknown-verb time. The
 * kernel-tier-to-/usr/-tier reference (the stored ext_obj_path) is the
 * architectural statement that the named /usr/-tier domain is fundamental
 * to eos-kernellib's operator surface.
 *
 * The caller whitelist that once lived here (allowed_callers) is now the
 * "admin_console.caller" capability in capabilityd, seeded at create();
 * _check_caller consults it through the inherited /kernel/lib/capability
 * helpers. The generalized capability model this registry's comment used
 * to anticipate has landed: the verb_* elevation helpers are the kernel
 * mediator for two capabilities -- they grant/revoke "merry.registrar"
 * directly against capabilityd (approve-registrar, unapprove-registrar),
 * and forward observer registration to MERRY's own _check_registrar gate
 * (register-observer, unregister-observer), each under the
 * admin_console.caller check.
 *
 * Registration of the kernel layer's own extensions is hardcoded at
 * create(). Future in-tree domains (Vault, Schema, HTTP operator
 * surfaces) extend the dispatch_table here and seed their own
 * capabilities in capabilityd.
 *
 * Applications outside the tree register through extend()/retract():
 * a /usr/-tier domain holding the "admin_console.extend" capability
 * (operator-approved via the console-ext verb below) registers
 * (verb, path, method) entries at boot or later, under the same
 * dispatch the built-in extensions get. The registered method sees the
 * console object as previous_program() exactly as cmd_session does,
 * so the same KERNEL() caller posture applies in the target.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>
# include <Merry.h>
# include <log.h>

# define HTTPSD		"/usr/System/sys/https_server"
# define IDENTITYD	"/usr/System/sys/identityd"
# define WEBAUTHND	"/usr/System/sys/webauthnd"
# define SESSIOND	"/usr/System/sys/sessiond"

inherit "/kernel/lib/capability";

/*
 * The console's built-in verb set: the /kernel/obj/admin_console command
 * switch plus the System operator surface's additions
 * (/usr/System/obj/user.c: issues, upgrade, hotboot, halt). An extension
 * may not register any of these -- the switches dispatch built-ins
 * before consulting the registry, so a shadow entry would sit silently
 * unreachable. Keep in sync with both switches.
 */
# define BUILTIN_VERBS ({ "code", "history", "clear", "compile", "clone", \
			  "destruct", "new", "cd", "pwd", "ls", "cp", "mv", \
			  "rm", "mkdir", "rmdir", "ed", "access", "grant", \
			  "ungrant", "quota", "rsrc", "people", "status", \
			  "swapout", "snapshot", "shutdown", "reboot", \
			  "issues", "upgrade", "hotboot", "halt" })

private mapping dispatch_table;

static void create() {
   dispatch_table = ([
      "observers":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_observers" ]),
      "cascade-depth":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_cascade_depth" ]),
      "batch-status":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_batch_status" ]),
      "dispatch-trace":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_dispatch_trace" ]),
      "register-observer":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_register_observer" ]),
      "unregister-observer":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_unregister_observer" ]),
      "query-approved-registrars":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_query_approved_registrars" ]),
      "approve-registrar":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_approve_registrar" ]),
      "unapprove-registrar":
         ([ "path": OBJ_MERRY_ADMIN_CONSOLE_EXT,
            "method": "cmd_unapprove_registrar" ]),
      "log":
         ([ "path": LOGD,
            "method": "cmd_log" ]),
      "log-level":
         ([ "path": LOGD,
            "method": "cmd_log_level" ]),
      "tls-cert":
         ([ "path": HTTPSD,
            "method": "cmd_tls_cert" ]),
      "identity":
         ([ "path": IDENTITYD,
            "method": "cmd_identity" ]),
      "webauthn":
         ([ "path": WEBAUTHND,
            "method": "cmd_webauthn" ]),
      "session":
         ([ "path": SESSIOND,
            "method": "cmd_session" ]),
      "capability":
         ([ "path": CAPABILITYD,
            "method": "cmd_capability" ]),
      "console-ext":
         ([ "path": ADMIN_CONSOLE_REGISTRY,
            "method": "cmd_console_ext" ]),
   ]);

   /*
    * Seed the admin_console.caller capability: the registered extension
    * library is the sole authorized entrypoint into the verb_* elevation
    * surface. This object is /kernel-tier so it grants directly. It is
    * compiled at boot by the driver (right after capabilityd), so the
    * seed -- and the extend() surface a domain initd may call -- are in
    * place before any domain boots.
    */
   CAPABILITYD->grant("admin_console.caller", LIB_MERRY_ADMIN_CONSOLE_EXT);

   /*
    * logd hosts its own operator verbs (cmd_log / cmd_log_level), so its
    * program path is the caller of verb_set_log_threshold below and must
    * hold admin_console.caller too.
    */
   CAPABILITYD->grant("admin_console.caller", LOGD);

   /*
    * identityd hosts the operator grant path for identity principals
    * (cmd_identity's grant / ungrant), so it reaches the
    * verb_grant_capability elevation helper below and must hold
    * admin_console.caller too.
    */
   CAPABILITYD->grant("admin_console.caller", IDENTITYD);
}

/*
 * query_dispatch: public lookup for admin_console::process() switch
 * default. Returns the dispatch entry or nil. Returning the path as a
 * string (rather than a resolved object) lets the caller decide whether
 * to find_object now or queue/lazy-load.
 */
mapping query_dispatch(string verb) {
   return verb ? dispatch_table[verb] : nil;
}

/*
 * query_verbs: public read-only enumeration of the registered verbs.
 * Exposed for operator-tier introspection (e.g., a future `help` verb)
 * and for verification.
 */
string *query_verbs() {
   return map_indices(dispatch_table);
}

/*
 * extend / retract: the application-facing registration surface. The
 * caller's program must live under /usr/<domain>/ for a domain holding
 * the "admin_console.extend" capability; the registered path must live
 * in that same domain (no planting verbs into another domain's
 * objects), and the method must carry the cmd_ prefix (the operator-verb
 * naming contract; no registering arbitrary LFUNs). Each entry records
 * its registrar domain, and only that domain may retract it -- the
 * kernel layer's own entries record no registrar and are not
 * retractable from here.
 */
void extend(string verb, string path, string method) {
   string caller, domain, pdomain;
   int i;

   caller = previous_program();
   if (sscanf(caller, "/usr/%s/%*s", domain) == 0) {
      error("admin_console_registry: extend wants a /usr/-tier caller");
   }
   require_member("admin_console.extend", domain);
   if (!verb || strlen(verb) == 0) {
      error("admin_console_registry: extend wants a verb");
   }
   for (i = strlen(verb); --i >= 0; ) {
      if (verb[i] == ' ') {
	 error("admin_console_registry: a verb is a single word: " + verb);
      }
   }
   if (sizeof(BUILTIN_VERBS & ({ verb }))) {
      error("admin_console_registry: verb shadows a console built-in: " +
	    verb);
   }
   if (dispatch_table[verb]) {
      error("admin_console_registry: verb already registered: " + verb);
   }
   if (!path || sscanf(path, "/usr/%s/%*s", pdomain) == 0 ||
       pdomain != domain) {
      error("admin_console_registry: path must live in the registrar's " +
	    "domain /usr/" + domain);
   }
   if (!method || sscanf(method, "cmd_%*s") == 0) {
      error("admin_console_registry: method must carry the cmd_ prefix");
   }
   dispatch_table[verb] = ([ "path": path, "method": method,
			     "registrar": domain ]);
}

void retract(string verb) {
   string caller, domain;
   mapping entry;

   caller = previous_program();
   if (sscanf(caller, "/usr/%s/%*s", domain) == 0) {
      error("admin_console_registry: retract wants a /usr/-tier caller");
   }
   require_member("admin_console.extend", domain);
   entry = verb ? dispatch_table[verb] : nil;
   if (!entry || entry["registrar"] != domain) {
      error("admin_console_registry: no extension registered by " + domain +
	    " under: " + (verb ? verb : "(nil)"));
   }
   dispatch_table[verb] = nil;
}

/*
 * NAME:	_emit()
 * DESCRIPTION:	route operator-verb output through the console user
 */
private void _emit(object user, string msg) {
   if (user) {
      user->message(msg);
   }
}

/*
 * NAME:	cmd_console_ext()
 * DESCRIPTION:	the console-ext operator verb, dispatched by this
 *		registry's own table:
 *		  console-ext                    -- extension entries and
 *		                                    approved domains
 *		  console-ext approve <domain>   -- grant admin_console.extend
 *		  console-ext unapprove <domain> -- revoke it and drop the
 *		                                    domain's registered verbs
 *		Unapprove sweeps the domain's live entries because a
 *		domain stripped of the capability could no longer retract
 *		its own leftovers (retract checks the same capability).
 */
void cmd_console_ext(object user, string cmd, string str) {
   string *parts, *verbs, *domains;
   mapping entry;
   int i, n;

   if (!KERNEL()) {
      error("Access denied");
   }

   parts = str ? explode(str, " ") - ({ "" }) : ({ });
   if (sizeof(parts) == 0) {
      string rows;

      verbs = map_indices(dispatch_table);
      rows = "";
      n = 0;
      for (i = 0; i < sizeof(verbs); i++) {
	 entry = dispatch_table[verbs[i]];
	 if (entry["registrar"]) {
	    rows += "console-ext:   " + verbs[i] + " -> " + entry["path"] +
		    "::" + entry["method"] + " (registrar " +
		    entry["registrar"] + ")\n";
	    n++;
	 }
      }
      domains = CAPABILITYD->query_principals("admin_console.extend");
      _emit(user, "console-ext: " + (string) n + " extension verb(s)\n" +
		  rows +
		  "console-ext: approved domains: " +
		  (sizeof(domains) ? implode(domains, ", ") : "(none)") +
		  "\n");
      return;
   }

   if ((parts[0] == "approve" || parts[0] == "unapprove") &&
       sizeof(parts) == 2) {
      if (parts[0] == "approve") {
	 CAPABILITYD->grant("admin_console.extend", parts[1]);
	 _emit(user, "console-ext: approved domain " + parts[1] + "\n");
      } else {
	 CAPABILITYD->revoke("admin_console.extend", parts[1]);
	 verbs = map_indices(dispatch_table);
	 n = 0;
	 for (i = 0; i < sizeof(verbs); i++) {
	    if (dispatch_table[verbs[i]]["registrar"] == parts[1]) {
	       dispatch_table[verbs[i]] = nil;
	       n++;
	    }
	 }
	 _emit(user, "console-ext: unapproved domain " + parts[1] +
		     ", dropped " + (string) n + " registered verb(s)\n");
      }
      return;
   }

   _emit(user, "usage: " + cmd + " [approve <domain> | unapprove <domain>]\n");
}

/*
 * _check_caller takes the caller_program captured at the public-LFUN
 * entry rather than calling previous_program() itself -- DGD's
 * previous_program() returns the program of the function that called
 * the CURRENT function, so a nested previous_program() in this helper
 * would return the verb_* program (this file) rather than the true
 * external caller. Same pattern as merry.c::_check_registrar.
 */
private void _check_caller(string caller_program) {
   require_member("admin_console.caller", caller_program);
}

/*
 * verb_* elevation helpers. Each forwards to the corresponding MERRY
 * LFUN with KERNEL elevation (the registry's program is /kernel/* so
 * KERNEL() and _check_registrar pass). The _check_caller gate ensures
 * the only entrypoint into this elevation surface is the registered
 * extension library, not arbitrary /usr/ code.
 */
nomask void verb_register_observer(object ob, string path, string timing,
                                   string source) {
   _check_caller(previous_program());
   MERRY->register_observer(ob, path, timing, source);
}

nomask void verb_unregister_observer(object ob, string path, string timing) {
   _check_caller(previous_program());
   MERRY->unregister_observer(ob, path, timing);
}

nomask void verb_remove_observer(object ob, string path, string timing,
                                 int index) {
   _check_caller(previous_program());
   MERRY->remove_observer(ob, path, timing, index);
}

nomask void verb_approve_registrar(string domain) {
   _check_caller(previous_program());
   CAPABILITYD->grant("merry.registrar", domain);
}

nomask void verb_unapprove_registrar(string domain) {
   _check_caller(previous_program());
   CAPABILITYD->revoke("merry.registrar", domain);
}

nomask void verb_set_max_cascade_depth(int n) {
   _check_caller(previous_program());
   MERRY->set_max_cascade_depth(n);
}

nomask void verb_set_dispatch_trace(int flag) {
   _check_caller(previous_program());
   MERRY->set_dispatch_trace(flag);
}

nomask void verb_set_log_threshold(int level) {
   _check_caller(previous_program());
   LOGD->set_threshold(level);
}

/*
 * verb_grant_capability / verb_revoke_capability: the operator path for
 * granting a platform capability to an authenticated identity. This is
 * the identity-principal analog of verb_approve_registrar -- the same
 * elevation shape (only the registered caller reaches it, and the
 * KERNEL()-gated CAPABILITYD->grant runs from this /kernel program), but
 * the principal is constrained to the identity namespace ("identity:"),
 * so the elevation surface is not a general-purpose grant of arbitrary
 * capabilities to arbitrary principals. Granting to a domain or program
 * principal stays a declared bootstrap-table entry or a /kernel caller,
 * never this operator path.
 */
private void _check_identity_principal(string principal) {
   if (!principal || sscanf(principal, "identity:%*s") == 0) {
      error("admin_console_registry: not an identity principal: " +
            (principal ? principal : "(nil)"));
   }
}

nomask void verb_grant_capability(string capability, string principal) {
   _check_caller(previous_program());
   _check_identity_principal(principal);
   CAPABILITYD->grant(capability, principal);
}

nomask void verb_revoke_capability(string capability, string principal) {
   _check_caller(previous_program());
   _check_identity_principal(principal);
   CAPABILITYD->revoke(capability, principal);
}
