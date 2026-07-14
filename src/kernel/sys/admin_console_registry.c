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
 * Registration is hardcoded at create(). Future domains (Vault, Schema,
 * HTTP operator surfaces) extend the dispatch_table here and seed their
 * own capabilities in capabilityd.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>
# include <Merry.h>
# include <log.h>

# define HTTPSD		"/usr/System/sys/https_server"
# define IDENTITYD	"/usr/System/sys/identityd"

inherit "/kernel/lib/capability";

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
   ]);

   /*
    * Seed the admin_console.caller capability: the registered extension
    * library is the sole authorized entrypoint into the verb_* elevation
    * surface. This object is /kernel-tier so it grants directly. It is
    * compiled lazily on first admin-console use -- the same flow that
    * first consults _check_caller -- so the seed is in place in time.
    */
   CAPABILITYD->grant("admin_console.caller", LIB_MERRY_ADMIN_CONSOLE_EXT);

   /*
    * logd hosts its own operator verbs (cmd_log / cmd_log_level), so its
    * program path is the caller of verb_set_log_threshold below and must
    * hold admin_console.caller too.
    */
   CAPABILITYD->grant("admin_console.caller", LOGD);
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
