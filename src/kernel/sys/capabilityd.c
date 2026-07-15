/*
 * capabilityd: the capability store and authority choke-point.
 *
 * Holds the mutable approved-sets for the set-bearing gating surfaces as
 * namespaced capabilities -- caps[<capability>][<principal>] -- keyed by
 * opaque principal strings, so heterogeneous principal types (a /usr
 * domain, a caller-program path) coexist under different capability names
 * without a shared key type. Every authority decision in the kernel layer
 * routes through is_allowed() here: /usr-tier surfaces call it directly;
 * System- and kernel-tier surfaces reach it through the inheritable
 * /kernel/lib/capability helpers. Mutation (grant/revoke) is gated
 * uniformly by KERNEL() -- only /kernel/* programs change any set.
 *
 * Bootstrap. A capability whose owning surface is /usr-tier cannot seed
 * itself: the surface neither passes the KERNEL() mutation gate nor may
 * inherit the elevation lib (the kernel restricts /kernel/lib inheritance
 * to creator=="System" objects). Such defaults are seeded in create(),
 * which the driver runs at boot before the first check: "merry.registrar"
 * and "http.binary_manager". By contrast "admin_console.caller" is seeded
 * by its kernel mediator (admin_console_registry) when that object
 * compiles. The bootstrap table is the documented ambient-authority seam.
 *
 * The store is a plain daemon mapping: like the per-surface sets it
 * replaces it persists across statedumps and is re-seeded on a cold boot
 * via create().
 */

# include <kernel/kernel.h>
# include <kernel/user.h>
# include <kernel/capability.h>

private mapping caps;		/* ([ capability : ([ principal : 1 ]) ]) */

static void create() {
   caps = ([
      /* Registrar domains trusted to register observers / script-spaces
       * across domain boundaries. The Merry dispatcher consults this on
       * every cross-domain registration, including at boot, so it must
       * exist before the Merry surface (which is /usr-tier and cannot
       * grant) ever runs. */
      "merry.registrar": ([ "System": 1, "admin_console": 1 ]),

      /* The binary-port manager authorized to accept HTTP connections.
       * http_server's accept path is /usr/System-tier and holds no grant
       * elevation, so it cannot seed its own identity principal. */
      "http.binary_manager": ([ USERD: 1 ]),

      /* Same authorization for the HTTPS bootstrap's accept path. */
      "https.binary_manager": ([ USERD: 1 ]),
   ]);
}

/*
 * is_allowed: boolean membership read. Public and side-effect-free -- the
 * silent path (an unauthorized caller learns only true/false) and the
 * basis for the throwing check. The principal is supplied by the calling
 * surface (captured at its public entry), never derived here.
 */
int is_allowed(string capability, string principal) {
   mapping set;

   if (capability && principal && (set = caps[capability]) && set[principal]) {
      return 1;
   }
   return 0;
}

/*
 * require_member: throwing membership check -- the uniform error posture.
 * The denial message lives here so every surface that throws on an
 * unauthorized principal (directly or via the lib forwarder) reports it
 * identically.
 */
void require_member(string capability, string principal) {
   if (!is_allowed(capability, principal)) {
      error("capability denied: principal " +
            (principal ? principal : "(nil)") + " lacks " +
            (capability ? capability : "(nil)"));
   }
}

/*
 * grant / revoke: capability-set mutation, gated uniformly by KERNEL().
 * Only /kernel/* programs mutate any set -- a single elevation point
 * rather than a per-capability grantor list or ambient-domain self-grant,
 * both of which would reintroduce the confused-deputy surface this avoids.
 */
void grant(string capability, string principal) {
   if (!KERNEL()) {
      error("capabilityd: grant not callable from outside /kernel");
   }
   if (!caps[capability]) {
      caps[capability] = ([ ]);
   }
   caps[capability][principal] = 1;
}

void revoke(string capability, string principal) {
   if (!KERNEL()) {
      error("capabilityd: revoke not callable from outside /kernel");
   }
   if (caps[capability]) {
      caps[capability][principal] = nil;
   }
}

/*
 * query_principals / query_capabilities: read-only introspection for state
 * inspection and a future admin verb. Public reads.
 */
string *query_principals(string capability) {
   mapping set;

   return (set = caps[capability]) ? map_indices(set) : ({ });
}

string *query_capabilities() {
   return map_indices(caps);
}

/*
 * delegable registry: a per-capability flag an operator flips before any
 * controller may delegate that capability to an agent. Default off -- no
 * capability ships delegable. The flag gates the delegation path in the
 * identity registry; the store above is untouched by it. Mutation is
 * KERNEL()-gated like grant/revoke; the read is public.
 */
private mapping delegable;	/* capability : 1 */

void set_delegable(string capability, int flag) {
   if (!KERNEL()) {
      error("capabilityd: set_delegable not callable from outside /kernel");
   }
   if (!capability || strlen(capability) == 0) {
      error("capabilityd: set_delegable wants a capability");
   }
   if (!delegable) {
      delegable = ([ ]);
   }
   delegable[capability] = flag ? 1 : nil;
}

int query_delegable(string capability) {
   return (delegable && capability && delegable[capability]) ? 1 : 0;
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
 * NAME:	cmd_capability()
 * DESCRIPTION:	the capability operator verb, dispatched by the kernel
 *		admin-console registry:
 *		  capability                        -- list the store
 *		  capability delegable <cap> on|off -- flip the delegable flag
 */
void cmd_capability(object user, string cmd, string str) {
   string *parts, *names;
   int i;

   if (!KERNEL()) {
      error("Access denied");
   }

   parts = str ? explode(str, " ") - ({ "" }) : ({ });
   if (sizeof(parts) == 0) {
      names = map_indices(caps);
      _emit(user, "capability: capabilities: " + (string) sizeof(names) +
                  "\n");
      for (i = 0; i < sizeof(names); i++) {
         _emit(user, "capability:   " + names[i] + " principals " +
                     (string) sizeof(query_principals(names[i])) +
                     (query_delegable(names[i]) ? " delegable" : "") + "\n");
      }
      return;
   }
   if (parts[0] == "delegable") {
      if (sizeof(parts) != 3 || (parts[2] != "on" && parts[2] != "off")) {
         _emit(user, "usage: " + cmd + " delegable <capability> on|off\n");
         return;
      }
      set_delegable(parts[1], parts[2] == "on");
      _emit(user, "capability: " + parts[1] + " delegable " + parts[2] +
                  "\n");
      return;
   }
   _emit(user, "usage: " + cmd + " [delegable <capability> on|off]\n");
}
