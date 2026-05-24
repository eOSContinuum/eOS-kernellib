/*
 * Admin-console verb registry. KERNEL-tier singleton consulted by
 * /kernel/obj/admin_console::process() in the switch-default branch
 * when an incoming verb is not in the hardcoded built-in set.
 *
 * #EX-3 selective extension (P3 layering). The registry holds two
 * pieces of state:
 *
 *   1. dispatch_table -- verb -> ([ "path": <ext_obj_path>,
 *                                   "method": <method_name> ])
 *      Consulted by admin_console::process() at unknown-verb time.
 *      The kernel-tier-to-/usr/-tier reference (the stored ext_obj_path)
 *      is the architectural statement that the named /usr/-tier domain
 *      is fundamental to eos-kernellib's operator surface.
 *
 *   2. allowed_callers -- caller-program -> 1
 *      Caller-program whitelist for the verb_* elevation helpers.
 *      Mutation verbs whose underlying MERRY API is KERNEL-gated
 *      (approve-registrar, unapprove-registrar) or capability-gated by
 *      caller-domain (register-observer, unregister-observer) call
 *      these helpers from /usr/Merry/lib/admin_console_ext; the helpers
 *      forward to the daemon with KERNEL elevation. This is a
 *      narrow-surface mini-capability-model; the Seed 11 future
 *      capability-model workstream generalizes the pattern across all
 *      kernel-layer subsystems (dispatcher approved-registrars +
 *      script-space registration + persist_helper + http_server auth
 *      + admin_console verb registration).
 *
 * Registration is hardcoded at create() for the MVA. Future domains
 * (Vault, Schema, HTTP operator surfaces) extend the dispatch_table +
 * allowed_callers entries here, or replace this object with dynamic
 * registration when Seed 11 lands.
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <Merry.h>

private mapping dispatch_table;
private mapping allowed_callers;

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
   ]);

   allowed_callers = ([
      LIB_MERRY_ADMIN_CONSOLE_EXT: 1,
   ]);
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
 * and for the verify path at #EX-3 close.
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
   if (!allowed_callers[caller_program]) {
      error("admin_console_registry: caller " + caller_program +
            " not authorized");
   }
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

nomask void verb_approve_registrar(string domain) {
   _check_caller(previous_program());
   MERRY->add_approved_registrar(domain);
}

nomask void verb_unapprove_registrar(string domain) {
   _check_caller(previous_program());
   MERRY->remove_approved_registrar(domain);
}

nomask void verb_set_max_cascade_depth(int n) {
   _check_caller(previous_program());
   MERRY->set_max_cascade_depth(n);
}

nomask void verb_set_dispatch_trace(int flag) {
   _check_caller(previous_program());
   MERRY->set_dispatch_trace(flag);
}
