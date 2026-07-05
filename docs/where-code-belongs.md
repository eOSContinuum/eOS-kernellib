# Where Code Belongs

An author adding behavior to this platform chooses twice: first between the two languages — plain LPC compiled at a capability tier, or a Merry script bound to a property — and then, for plain LPC, among several placement shapes (an inheritable library, a daemon, a cloneable, a pure-function utility). Each choice has a structural answer, not a stylistic one: the host driver's inheritance rules, the sandbox boundary, and the platform's authority discipline decide most cases. This document is the decision guidance, distilled from the choices the platform itself made while building its own surfaces.

**Audience**: an application author deciding where a new piece of behavior should live, and a platform contributor placing a new kernel- or System-tier facility. Assumes the tier vocabulary from `docs/architecture.md`; `docs/application-authoring.md` covers the mechanics of each shape (domain layout, initd, access) once the placement is chosen.

## The first fork: plain LPC or Merry script

Everything on the platform is ultimately LPC — a Merry script compiles to a wrapped LPC function (`docs/merry-applications.md` Compiling a script). The fork is not about capability of expression; it is about **how the behavior is bound, trusted, and changed**.

**Bind a Merry script to a property when the behavior is data-shaped:**

- **It belongs to an object, not to the code base.** A script lives in the object's own property table (`merry:<mode>:<signal>`), travels with the object through statedump and restore, and is inherited by every descendant through the UrHierarchy ancestry walk. Behavior that varies per-object or per-subtree — a reaction here, a policy there — is property data, and Merry is the platform's format for behavior-as-data.
- **It reacts to state change.** The property-change dispatcher's observer surface *is* Merry: `register_observer` compiles Merry source and binds it under `merry:on:<path>:<timing>` (`docs/signal-applications.md`, `docs/observers.md`). Reacting to a write is the canonical script-shaped job.
- **It changes at runtime without compile access.** A script is installed by writing a property — from the console, from other code, from an operator session — with no file under `src/`, no recompile, no deploy step. Behavior that operators or the application's own users author lands here.
- **It must not be trusted with the platform.** The sandbox denies the escape- and effect-shaped kfuns — object lifecycle, filesystem, networking, `call_other` on objects — with a 51-entry deny set enforced at evaluation (`docs/merry-language.md` The sandbox surface). A script author can compute over the arguments and the bound object's state, call whitelisted merryfuns, and schedule timers; nothing else. That containment is the point: script-shaped behavior is the platform's surface for code whose author does not hold tier authority.

**Write plain LPC when the behavior is structure-shaped:**

- **It needs the denied surface.** Anything that clones, destructs, compiles, reads or writes files, opens or serves connections, or calls arbitrary objects is outside the sandbox by design. Services, transports, daemons, and persistence machinery are LPC.
- **It is the application's skeleton.** Domain layout, the initd, the clonables that carry state, the libraries other code inherits — the structural inventory `docs/application-authoring.md` describes is all compiled LPC. Scripts decorate this skeleton; they do not replace it.
- **It is a shared algorithm.** Code called from many places, performance-sensitive code, and code with real inheritance structure belongs in the compiled tree where the driver's tier enforcement, the upgrade cascade, and the object manager see it.

The boundary case is behavior that starts script-shaped and grows structural — a reaction that accretes logic until it wants library calls the sandbox denies. The migration is mechanical (a script's body is already near-LPC), and the direction is one-way by design: move the logic into a compiled library or daemon in the owning domain, and leave behind a thin script that delegates through a script space (`docs/merry-applications.md` Phase 5 — LabelCall), which is the sanctioned bridge from script code to a compiled handler.

## Placing plain LPC: shape follows consumer

Four shapes, and the deciding question for each is *who calls it, and how*.

### Pure functions: a `/lib/util` library

Stateless helpers — encoding, parsing, string and value manipulation — belong in a private-inherit library under `/lib/util/`, beside `ascii`, `lpc`, and `coercion`. Consumers inherit the library and the `static` helpers become part of their own program; there is no daemon to boot-order against, no `call_other` overhead, and no shared state to protect. The coercion codec chose this shape over a daemon precisely because pure functions gain nothing from a process-like home (`docs/kernel-libraries.md` Utility libraries).

### Facilities `/usr`-domain code must reach: a daemon, not an inheritable kernel library

The host driver restricts `/kernel/lib` inheritance to objects whose creator is `"System"` (`src/kernel/sys/driver.c`, the inherit gate). A `/usr/<Domain>` object cannot inherit a kernel-tier library — so a facility shipped as an inheritable `/kernel/lib` splits into two front doors the moment a `/usr`-tier consumer appears. The capability library carries exactly that scar: System-tier surfaces inherit the check face, while `/usr`-tier surfaces call the store daemon directly (`docs/capability.md` The tier-split access path).

The lesson the platform's later facilities applied: **a new facility that `/usr`-domain code must reach ships as a daemon**, reached by `call_other` through `find_object` — one front door, no tier split, because the inheritance restriction never engages. The logging facility is the model: `sysLog` / `info` / `debugLog` forward to the `logd` daemon from any tier (`docs/operations.md` Logging and diagnostics). The observer query surface made the same choice: daemon-direct LFUNs on the Merry daemon rather than an inheritable API library (`docs/observers.md`).

An inheritable library remains right when its consumers are all System- or kernel-tier *and* the helpers should extend the inheritor's own surface rather than stand as a separate callable — the capability check face, kept deliberately for its three System-tier inheritors, is the worked example.

### Instances and singletons: the directory conventions

Cloneables under `obj/`, singleton daemons under `sys/`, LWO value types under `data/`, inheritables under `lib/` — the driver and kernel enforce these, and `docs/application-authoring.md` Domain layout covers them. Placement doctrine adds one steer: state that must exist once per domain is a `sys/` daemon; state that exists per-instance is a clonable plus the properties it carries; and a value that crosses dataspaces is an LWO, copied rather than shared (`docs/code-lifecycle.md` LWO instantiation).

## Authority: one choke-point, never inline checks

When new code needs a gate — "may this caller do this?" — the answer is never a fresh inline `previous_program()` comparison. The platform routes every authority decision through the capability store's single membership check (`capabilityd::is_allowed` / `require_member`), whether reached by inheriting the check face or by calling the daemon directly (`docs/capability.md` The mechanism). One store, one denial message, one place a future mediation mode attaches. Scattered inline checks are how the pre-consolidation platform accumulated six heterogeneous gates; the consolidation exists so that number stays one.

The same discipline governs writes. Property mutations go through the dispatched `set_property` path — where observers fire, gates apply, and atomicity holds — not through `set_raw_property`, whose bypass is scoped to the dispatcher's own internals and deliberate bootstrap (`docs/capability.md` Limitations). An import is a write: the marshaling accessors decode and then call `set_property`, accepting observer side effects as semantically correct rather than routing around the choke-point (`docs/schema.md` Property-table marshaling).

## Composition and interception: put masks at call_other seams

LPC's function binding decides where an extension point can live, and it is easy to guess wrong. Within a compiled program, a call to a function *defined* in that same program binds statically: a child that redefines an inherited, defined function does **not** intercept the ancestor's internal calls to it. Only two mechanisms dispatch to the most-derived definition:

- **An undefined prototype.** A library that declares `int message(string str);` without a body forces every internal call through the inheritor's implementation — the console library's pattern for delegating output to whatever object inherits it.
- **`call_other` self-dispatch.** `call_other(this_object(), "cmd_" + verb, ...)` resolves through the object's function table at call time, so the most-derived `cmd_*` definition wins regardless of where the dispatch loop lives.

The consequence for placing an extension: **interpose where the call already crosses one of those seams.** The console is the platform's worked example — the ~250-line cloneable that self-dispatches verbs is where the clone-addressing masks landed (each mask translates its argument and delegates to the inherited `::cmd_*`), while the ~2,300-line console library underneath stayed untouched and composition-free (`docs/admin-console.md` Target resolution). Extending a large library by inheriting it and redefining its internals looks equivalent and is not: the library's own internal calls will keep binding to the original definitions, and the "override" silently applies only to external entry points.

So the doctrine: keep large libraries free of composition assumptions, keep the composition seam in the small object that inherits them, and when adding an extension point to new code, make the seam explicit — a prototype the inheritor must supply, or a self-dispatching `call_other` — rather than relying on redefinition.

## The promotion path

Placement is not permanent. A pattern proven at the application tier is promoted into the platform when — and only when — a trigger condition is met: a real consumer demanding the surface, not an anticipated one (`docs/runtime-platform-roadmap.md` How to read this roadmap). The same discipline reads in reverse for authors: do not place code kernel- or System-side because it might someday serve everyone. Build it in the owning domain, let demand prove the shape, and let the roadmap's trigger discipline decide the promotion. The application-tier boundary section of the roadmap lists the patterns the platform has committed to *never* absorbing — domain events, verb dispatch, auto-tracked reactivity — which is placement guidance of the strongest kind: those belong to the application, permanently.

## Where to next

- [application-authoring.md](application-authoring.md) — the mechanics of each shape: domain layout, the initd, owner and access, tick budgets, `call_touch` upgrades.
- [merry-applications.md](merry-applications.md) — the script-bearing-object contract, the storage convention, the invocation surface, and what the sandbox forbids.
- [merry-language.md](merry-language.md) — Merry-the-language: dialect restrictions, the four extensions, the full 51-entry sandbox surface.
- [capability.md](capability.md) — the authority choke-point, the tier-split access path, and the capability model's limitations.
- [architecture.md](architecture.md) — the capability tiers and path conventions this document's placement rules build on.
- [runtime-platform-roadmap.md](runtime-platform-roadmap.md) — the trigger discipline governing promotion from application tier to platform surface.
