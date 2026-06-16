/*
 * /kernel/lib/capability: inheritable capability-check helpers.
 *
 * Inherited by System- and kernel-tier gating surfaces. (/usr/<domain>
 * surfaces such as Merry cannot inherit a /kernel/lib -- the kernel
 * restricts that to creator=="System" objects -- so they call the same
 * is_allowed / require_member choke-point on capabilityd directly.)
 *
 * The helpers forward to capabilityd so the store and the denial message
 * keep a single home. The `principal` argument is the forward-compat
 * seam: today an ambient-derived string (a domain, a program path); a
 * future facet mode could present a held capability handle here without
 * changing any call site. Helpers are static so they extend the inheriting
 * object's own surface rather than becoming an externally callable one.
 */

# include <kernel/capability.h>

/*
 * is_allowed: boolean membership read -- the silent path (no throw).
 */
static int is_allowed(string capability, string principal) {
   return CAPABILITYD->is_allowed(capability, principal);
}

/*
 * require_member: throwing membership check -- the uniform error posture.
 */
static void require_member(string capability, string principal) {
   CAPABILITYD->require_member(capability, principal);
}

/*
 * require: fixed-principal assertion for surfaces with no stored set (e.g.
 * the persist dump/exit creator gate). Uniform throw posture; consults no
 * store.
 */
static void require(int cond, string msg) {
   if (!cond) {
      error(msg);
   }
}
