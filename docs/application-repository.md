# Your application beside the platform tree

Your service's source does not live in a fork of this repository. This document is the repo-and-tree posture for an application team: what your own repository owns, how its contents land on an eOS-kernellib checkout for development, CI, and deployment, and how platform updates flow underneath a deployed application. The tutorials deliberately build inside the platform clone (`first-application.md` has you create `src/usr/KV/` in the repository you booted from); this document is the graduation step -- the same domains, moved to a home of their own.

**Audience**: an application team setting up the real repository for a service the tutorials prototyped, and the operator wiring its CI and deploy flow.

## The split

The platform checkout stays pristine; your repository owns everything specific to your service:

- Your tier-E domain directories -- one `<App>/` per domain, each the exact tree that lands at `src/usr/<App>/` (`application-authoring.md` Domain layout owns the internal shape).
- Any System-tier overlay file your service carries (most services carry none -- see below).
- Your production `.dgd` configuration (splice your sizing into a copy of `example.dgd` per `configuration.md` Limits and capacity, the production-shape starting point).
- Your harness runner and its sentinel expectations (below).
- Your deploy script and your repository's ignore file -- when the secret recipe (`common-tasks.md` Provision an application secret out of source) says "add the path to your repository's ignore file", this repository is the one it means.

The platform checkout is a deployment target, not your working tree. The clean-slate reset (`common-tasks.md` Reset a development checkout to a clean slate) already treats deployed domain mounts as removable residue, and that is the correct reading: your repository is the source of truth for your domains, and a checkout can be reset out from under them at any time.

## A recommended layout

```text
your-app/
    domains/
        Orders/            -- lands at src/usr/Orders/ (initd.c, lib/, obj/, sys/, data/)
        Billing/           -- a second domain, if the service splits ownership
    overlay/
        sys/               -- System-tier files, only if carried (see below)
    config/
        production.dgd     -- your sized configuration
    harness/
        run-service.sh     -- your clean-slate/boot/assert runner
    scripts/
        deploy.sh          -- the copy step below
```

The names are yours; the load-bearing property is that each `domains/<App>/` is byte-for-byte the tree that lands at `src/usr/<App>/`, so deployment stays a plain recursive copy with no rewriting.

## Composing onto a checkout

Development, CI, and production deployment all use the mechanism the platform already prescribes: deploy-by-copy, then a cold boot -- the initd iteration that registers a new domain runs only there (`operations.md` Day 0 step 6).

```sh
rsync -a --delete your-app/domains/Orders/ "$PLATFORM/src/usr/Orders/"
```

After the first cold boot the loop tightens: edit in your repository, re-run the copy, and `compile <file.c>` the changed master from the console -- recompiles preserve object state, so the copy-then-compile loop keeps your fixtures (`debugging-applications.md` The working environment, plainly). A new file, a new domain, or a changed `initd.c` needs the cold boot that re-runs registration.

## System-tier overlay files

Most applications carry none. The one need the tutorials surface -- boot-time access grants -- is provisioned once from the console instead, and persists in the kernel's saved access data across reboots and restores (`common-tasks.md` Grant another domain access to your files).

Carry an overlay file when your service genuinely needs code at tier C: a System-gated call your domains must reach, and no platform surface exposes, is the concrete case (`dump_state` was the canonical example until the platform grew its capability-gated dump-only surface, `persist_helper->trigger_dump()` -- the durability recipe now routes through that, `common-tasks.md` Make one write durable at acknowledge time, and an overlay is only for calls with no such surface). An overlay file is an ordinary `.c` your deploy step copies under `src/usr/System/sys/` (the platform overlay, `source-map.md`). One mechanism to know before relying on it: the System initd compiles a fixed list of its own objects at boot -- it does not sweep `sys/` for new files -- so a from-source cold boot does not compile your overlay file. Compile it explicitly from the console after such a boot (`compile /usr/System/sys/<name>.c`; scriptable headlessly the same way the harness drives console verbs, `scripts/README.md`). The compiled master then persists in the image across every snapshot restore; only the next from-source cold boot repeats the step.

Overlay files sit outside your domains' ownership story: review one with the same care as a kernel change, keep them few, and name them after your service so a platform update never collides with them.

## Your harness runner and CI

`scripts/run-example.sh` resolves its examples through a hardcoded profile table -- there is no discovery, and adding your domain means editing the platform's script (`application-authoring.md` Adapting run-example.sh to a new domain). Your repository therefore carries its own thin runner with the same shape -- clean-slate, copy your domains on, boot, count your sentinel driver's OK lines (`application-authoring.md` Testing your application owns the sentinel pattern) -- written fresh against that recipe, or maintained as a patched copy of `run-example.sh` with your case line added by your deploy step.

CI is that runner, headless: build the driver at the pinned commit (seconds, cacheable -- `getting-started.md` states the measured cost), check out the platform at your pinned ref, copy your domains on, boot, and assert the sentinel count. "An application team's CI is the same sweep with its own example profile added" (`debugging-applications.md`).

## Deploying and updating production

Production deployment is the release ladder in `changing-a-running-system.md`; its step that brings "the domain's source to the release state (a git checkout or copy on the host)" is your repository landing through the same copy script. The reverse flow -- a platform update underneath your deployed application -- is a pin advance: your repository records the platform ref it last validated; to update, advance the pin in a rehearsal environment first (the restored-copy rehearsal, `changing-a-running-system.md`), re-run your CI runner against the new ref, then roll production up the same ladder.

## Assembling a production application

The tutorials and reference docs each teach one pattern in isolation. Standing up a real service means sequencing them: each step below depends on the ones before it, and each is one or two sentences plus the doc that owns its mechanics -- this list owns only the order.

1. **Split the repository.** Give the service its own repository per The split above, with each `domains/<App>/` a byte-for-byte copy of what lands at `src/usr/<App>/` (A recommended layout above). Everything that follows assumes this posture, not a fork of the platform checkout.
2. **Lay out the domain.** One directory per domain under `src/usr/<App>/` with the `lib`/`obj`/`sys`/`data` convention, and an `initd.c` that compiles the domain's boot-time objects (`application-authoring.md` Domain layout; The initd's role).
3. **Add the sentinel suite from day one.** A `sys/test.c` boot-time driver, deferred past every domain's `initd`, writing `OK`/`FAIL` lines to a result log -- write it alongside the domain's first objects, not after (`application-authoring.md` Testing your application; `common-tasks.md` Add a boot-time test driver to your domain).
4. **Wire grants and any System-tier overlay.** Provision the cross-domain access the domain needs from the console (`common-tasks.md` Grant another domain access to your files), and carry a System-tier overlay file only if a System-gated call forces one (System-tier overlay files above).
5. **Move secrets out of source.** Anything the domain needs at runtime that must not sit in the repository -- an API key, a credential -- lives as a host file under the domain's data directory, read at use time (`common-tasks.md` Provision an application secret out of source).
6. **Expose a health route.** A status route on the domain's own transport, answering with the capacity-headroom counts a monitor can poll with no console login (`common-tasks.md` Expose a health check for monitoring).
7. **Decide durability per write.** For each write the service acknowledges to a client, choose snapshot-on-critical-path or edge-file persistence before it ships (`common-tasks.md` Make one write durable at acknowledge time); a domain persisting more than one entity kind through the Vault models each kind's schema at this point too (`vault-applications.md` Application layout).
8. **Add the operator surface the service actually needs.** Start with a capability-gated route on the domain's own transport, and reach for a registered console verb only once it has earned its place there (`application-authoring.md` Give your application an operator surface owns the preference order; `common-tasks.md` Add an operator verb for your application has the registration mechanics).
9. **Order Day 0 and hand off to a supervisor.** The claim-admin-first, transport-security-before-traffic, extensions-are-cold-boot-facts ordering (`operations.md` Day 0: standing up a production deployment), then a process supervisor that sends `SIGTERM` and gives the stop timeout room for the snapshot (`operations.md` Running under a supervisor).

What this checklist is not: a tutorial, and not a substitute for reading the docs it links. It also does not resolve into a shipped example -- the full production-shaped composition (users, sessions, transport, multiple persisted entity kinds, an operator surface) is deliberately not one of the bundled reference applications; each of those exercises one primitive at a time, and the omission is stated plainly at `application-authoring.md` Reference applications.

## Where to next

- `application-authoring.md` -- everything inside a domain directory: layout, data modeling, the initd, testing.
- `operations.md` Day 0 -- the production sequence your deploy step slots into.
- `changing-a-running-system.md` -- releases, rollbacks, and the restored-copy rehearsal.
- `common-tasks.md` -- the grant, secret, durability, and reset recipes this document routes.
