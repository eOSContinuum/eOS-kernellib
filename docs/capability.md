# Capability Library

The capability library is the kernel layer's single authority choke-point: one store and one membership check behind every gating surface that asks "may this caller do this?" It replaces the heterogeneous per-subsystem gates the platform shipped with — each surface previously carried its own allowlist, its own mutator, and its own denial posture — with a shared store (`/kernel/sys/capabilityd`) and an inheritable check face (`/kernel/lib/capability`). Six gating surfaces route through it.

This document covers what the library is, what "capability" means here and what it does not mean, the mechanism, the tier-split access path the kernel's own inheritance rules force, the bootstrap and persistence lifecycle, the design choices behind the shape, and the limitations a reader auditing the platform's authority model should know.

**Audience**: a kernel- or System-tier author adding or auditing an authority gate, or an architect evaluating the platform's capability model. Assumes `docs/architecture.md` for the tier vocabulary and `docs/runtime-primitives.md` §2 for the capability-separation primitive this library implements.

## What "capability" means here — and what it does not

The platform's capability separation is **tier and owner-identity mediation**, not strict, no-ambient-authority object-capability. The distinction matters for a reader who arrives with the object-capability literature (KeyKOS, EROS) in mind.

What the platform provides is capability-*shaped*: every privileged operation is reachable only through a mediating `/kernel/` object that checks the caller before acting — the lineage the orthogonal-persistence literature pairs with capability-based access control. But the check is enforced by **ambient authority** — the caller's tier, its owning domain, and the `previous_program()` call chain — not by an unforgeable, held reference that *is* the sole authority to act. A caller's rights follow from who it is, not from what reference it holds.

Strict object-capability — per-call mediation of every reference, authority carried only by held references, revocation and per-reference attenuation — is not reachable in LPC alone. The host driver exposes no per-`call_other` security hook; object references are unforgeable but confer no sole authority; there is no revocation or attenuation primitive. Achieving it would require host-driver changes that are out of scope for a library built *on* the driver. Pure-LPC facet or caretaker patterns — an object that privately holds a reference and exposes attenuated methods — are possible but bypassable whenever the underlying object is reachable by name. So tier-mediation is the faithful kernel-tier realization of the documented model, not a shortfall against an object-capability target the platform was trying to hit.

## The mechanism

Two artifacts, named by canonical `#define` in `<kernel/capability.h>`:

**`/kernel/sys/capabilityd`** — the store and the choke-point. It holds the mutable approved-sets as namespaced capabilities, `caps[<capability>][<principal>]`, keyed by opaque principal strings so heterogeneous principal types (a `/usr/` domain, a caller-program path, an object name) coexist under different capability names without a shared key type. Its surface:

- `int is_allowed(string capability, string principal)` — boolean membership read. Public and side-effect-free; the silent path, and the basis for the throwing check.
- `void require_member(string capability, string principal)` — throwing check. Errors with one uniform message (`capability denied: principal <p> lacks <c>`) so every store-backed surface that throws reports identically. The message lives here, in one place.
- `void grant(string capability, string principal)` / `void revoke(string capability, string principal)` — mutation, gated uniformly by `KERNEL()`. Only `/kernel/*` programs change any set.
- `string *query_principals(string capability)` / `string *query_capabilities()` — read-only introspection. Public.

**`/kernel/lib/capability`** — the inheritable face. `static` helpers that forward to `capabilityd` so the store and the denial message keep a single home: `is_allowed` and `require_member` (the store-backed checks), plus `require(int cond, string msg)` for fixed-principal surfaces that have no stored set and supply their own condition and message. Because the helpers are `static`, they extend the inheriting object's own surface rather than becoming externally callable.

Every authority decision in the kernel layer flows through this one `is_allowed` / `require_member` pair. The `principal` argument is a deliberate extension seam: today it is an ambient-derived string (a domain, a program path, an object name), but it is the single place a future mediation mode could present a held capability handle without touching any call site.

## The tier-split access path

The check API has two front doors — not because the policy differs by tier, but because the kernel's own inheritance rules force it.

The host driver restricts `/kernel/lib` inheritance to objects whose creator is `"System"`. A `/usr/<domain>` object — `/usr/Merry`, for instance — cannot inherit `/kernel/lib/capability`. So:

- **System- and kernel-tier surfaces** (the persistence helper, the HTTP acceptor, the console registry) inherit `/kernel/lib/capability` and call the inherited helpers.
- **`/usr/`-tier surfaces** (the Merry dispatcher's registrar gate and its observer-property gate) call the same `is_allowed` / `require_member` methods on `capabilityd` directly.

One store, one membership function, one denial message; only the path to it differs. A further wrinkle: the kernel auto (`/kernel/lib/auto`) cannot inherit the library either — auto is the base every object implicitly inherits, the library included, so inheriting it there would be circular. The persistence helper surfaces the uniform check at the System tier while the auto's own `creator == "System"` test remains the foundational backstop beneath it.

## The six gating surfaces

| Surface | Capability | Check | Denial |
|---|---|---|---|
| Dispatcher registrar approval | `merry.registrar` | `is_allowed` (with `KERNEL()`-pass and self-domain match inline) | throw |
| Script-space registration | `merry.registrar` (shared) | same registrar gate | throw |
| Observer-property writes (`merry:on:*`) | `merry.registrar` (shared) | registrar gate on the dispatched write | throw |
| Persistence dump-and-exit | (none — fixed principal) | `require(owner == "System")` | throw |
| HTTP acceptor binding | `http.binary_manager` | `is_allowed` against the manager's object name | silent nil |
| Console verb-elevation callers | `admin_console.caller` | `require_member` | throw |

Two postures coexist by design. Five surfaces throw — the daemon-diagnostic posture, where an unauthorized call is a programming error worth surfacing. The HTTP acceptor keeps a **silent nil**: erroring on every unauthorized connection attempt, port-scan probes included, buys nothing, so the accept path drops the connection quietly. The dual-check API (`require_member` for the throwers, `is_allowed` for the silent path) serves both with one mechanism, and the migration changed no surface's observable behavior.

The observer-property gate deserves a note. A direct write to a `merry:on:*` (or `merry:on-inherit:*`) property — the keys that store observer registrations — is gated by the *same* registrar capability as `register_observer`, applied on the dispatched-write path. The writer's program is captured at the public `set_property` / `batched_set` entry and threaded to the dispatcher, which fails closed if it is absent. This closes the path where a raw property write would otherwise install an observer registration that `register_observer`'s own gate would have refused.

## Bootstrap and lifecycle

Capabilities whose owning surface cannot seed itself are seeded in `capabilityd::create()` — the bootstrap table:

- `merry.registrar` → `System`, `admin_console`. The dispatcher consults this on every cross-domain registration, including at boot, and the Merry daemon is `/usr/`-tier: it neither passes the `KERNEL()` mutation gate nor may inherit the elevation library, so it cannot seed its own set.
- `http.binary_manager` → the connection manager (`userd`). Same reason: the acceptor is System-tier but holds no grant elevation.

`admin_console.caller` is seeded differently — by the console registry's own `create()`, because that object is kernel-tier and grants directly when it compiles. The driver compiles `capabilityd` at boot before the first check runs, so each table is in place in time.

This bootstrap table is the documented ambient-authority seam: the initial grants are not themselves earned through a capability check — they are declared. A reader auditing the platform's root trust reads `capabilityd::create()` and the console registry's `create()`.

The store persists like the per-surface sets it replaced. It survives statedumps; a cold boot re-seeds the bootstrap table via `create()`; recompiling `capabilityd` re-runs `create()`, re-seeding the static grants while dropping any dynamic grants added at runtime. One consequence is worth calling out: because the store is a standalone daemon decoupled from the Merry daemon's lifecycle, hot-reloading Merry no longer disturbs the registrar capability state — the capability table outlives a Merry recompile, a small win for the hot-reload primitive.

## Design choices

- **A hybrid of an inheritable check and a shared store**, rather than a per-surface library or a single central daemon owning every check. The shared store gives cross-surface introspection and the dynamic registration the console registry had anticipated; keeping the check inheritable (where inheritance is allowed) avoids normalizing four heterogeneous principal types into one key, and let the migration proceed surface by surface.
- **Uniform `KERNEL()` mutation authority**, rather than a per-capability grantor list or an ambient-domain self-grant. A single narrow elevation point — only `/kernel/*` grants or revokes — avoids the confused-deputy surface that spreading mutation authority across domains would reintroduce, and it preserves the pre-migration behavior of the set-bearing surfaces exactly.
- **A dual-check API with per-surface posture preserved.** Every surface that threw still throws (now with one uniform message for the store-backed checks); the one surface that dropped silently still does. No observable behavior changed.

## Limitations

The model is honest about its boundaries:

1. **Ambient authority.** Rights follow from the caller's tier and domain, not from a held, unforgeable reference. There is no per-call mediation and no per-reference attenuation. (See "What capability means here" above.)
2. **The check seam is split.** System- and kernel-tier surfaces reach the check by inheriting the library; `/usr/`-tier surfaces call the daemon directly. A future held-reference or facet mode would have to interpose at both front doors, not one.
3. **"Single choke-point" means a single point of *dynamic membership*.** The membership *set* lives in one place, but the structural always-pass rules — a `/kernel/*` caller passes, a caller acting on its own domain passes — necessarily stay inline at each surface, because they are properties of the call, not entries in a set.
4. **The capability table is world-readable.** `query_principals` and `query_capabilities` are public reads, so any `/usr/` code can enumerate who holds what. The table is an authorization record, not a secret; no principal's membership is confidential.
5. **The observer-property gate covers the dispatched path only.** `set_raw_property` is `nomask` and writes straight to storage, deliberately bypassing the dispatcher and its gate. The bypass is bounded by ordinary object-reference access control — a caller still needs a reference to the target and the tier to call `set_raw_property` on it — but it is a bypass, used intentionally during early bootstrap and inside the dispatcher itself.
6. **The bootstrap table is declared trust.** The seed grants are not earned through a check; they are the platform's root trust assumptions — auditable, but not themselves mediated.

## Toward stricter object-capability

The limitations above sketch a future direction rather than a defect to patch in place: a no-ambient-authority mode at the untrusted boundary — the Merry sandbox and the observer surfaces — where authority would be carried by held, attenuable, revocable references (facet or caretaker patterns) instead of by ambient tier identity. Pure LPC reaches only a partial, bypassable version of this; a faithful implementation needs host-driver primitives the platform does not have today (per-call mediation, reference revocation, designation-as-authority). It is a research direction for a later wave, not a near-term commitment — and the extension seam in the check API (the `principal` argument) is the place it would attach.

## Where to next

- [runtime-primitives.md](runtime-primitives.md) §2 — the capability-separation primitive this library implements, with the tier model and demonstration status.
- [architecture.md](architecture.md) — the capability-tier vocabulary (A through E) and the access-check model the library builds on.
- [dispatcher.md](dispatcher.md) — the registrar gate and the observer-property write gate in the context of the Merry dispatcher.
- [admin-console.md](admin-console.md) — the operator surface, including the verb-elevation registry that seeds and uses `admin_console.caller`.
