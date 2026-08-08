# Adopting the platform

The twelve-week shape of a real trial: from a first boot to a decision to migrate, expand, or step back, naming the doc, example, or script that serves each step and saying plainly where the platform leaves you to build your own. It picks up where [`evaluating.md`](evaluating.md) stops. That page is the fit decision; this one is what a team does after deciding the fit is worth testing.

Like `evaluating.md`, this page restates other documents and links them; nothing here is the authoritative statement of anything. Where the two disagree, the linked depth doc is right.

**Audience**: a builder or team lead who has read [`evaluating.md`](evaluating.md), judged the platform worth a real trial, and wants the trial's shape before committing weeks to it.

This is not a sales process, and the twelve weeks are not a funnel. Stepping back at week 12 with measurements is a real outcome: the data says where the surface needs work, which is worth more to the project than a migration made on optimism.

## The shape of the trial

| Stage | Weeks | What you finish with | What the platform does not give you |
|---|---|---|---|
| [Run it locally](#week-1-run-the-runtime-locally) | 1 | A booted platform, an object of your own surviving a restart, the primitive proofs passing on your hardware | Nothing material. This stage is served end to end. |
| [Translate one workflow](#weeks-2-3-translate-one-workflow) | 2-3 | One real workflow running on the platform, and your own list of what was direct and what was not | A worked port. Every example here is greenfield; no reference translation exists. |
| [Run it in parallel](#weeks-4-6-run-it-in-parallel) | 4-6 | Measurements from both systems against the same inputs | A comparison harness pointed at your application, and any cost model at all. |
| [Decide](#weeks-7-12-decide) | 7-12 | A migrate, expand, or step-back decision backed by your own data | A committed response time. See [Either way, tell the project](#either-way-tell-the-project). |

The stages are sequential because each consumes the previous one's output, but the weeks are a shape rather than a schedule: a team that already knows the workload can compress stages 1 and 2, and a team without a candidate workflow should not start stage 2 at all.

## Before you start

Three things are cheaper to settle now than in week 4.

- **The fit decision.** [`evaluating.md`](evaluating.md) Fit and anti-fit lists six conditions under which the platform is the wrong choice by design. If one of them holds for the workload you have in mind, a trial will spend twelve weeks confirming it. Pick a different workload or a different platform.
- **The crypto module, if you will need it.** Identity, sessions, native TLS, and the agent substrate all require the lpc-ext crypto module; without it they stand down cleanly and their regression steps report documented skips. Build it in week 1, not week 4, because extensions are cold-boot facts: a module added at a restore boot reaches the kfun table but not the platform daemons ([`operations.md`](operations.md#loading-host-driver-extensions) Loading host-driver extensions). The build is one `make crypto` ([`../scripts/README.md`](../scripts/README.md) Full regression sweep names it among the prerequisites).
- **One person who will learn LPC.** In-image code is LPC, and no amount of transport-boundary integration removes that. [`lpc-essentials.md`](lpc-essentials.md) is the bridge, and it is short, but it is not a language your team knows. [`evaluating.md`](evaluating.md) Adoption risks, priced states the ramp honestly.

## Week 1: run the runtime locally

Three things, in order: get it running, write something of your own, and watch the primitive proofs pass on your own hardware.

### Boot it

[`getting-started.md`](getting-started.md#zero-to-a-passing-proof) Zero to a passing proof is a single block: clone the driver, build it, clone this repository, run one example through a real dump-and-restart cycle. The driver build has no dependency fetch and completes in well under a minute on recent hardware. If you would rather not put a C toolchain on the host, Run it in a container builds the pinned driver inside the image and runs the same proof.

Then boot it yourself rather than through the harness: [`getting-started.md`](getting-started.md#boot-it-yourself-the-configuration-and-the-ports) Boot it yourself walks the configuration copy and the two bound ports, and [`admin-console.md`](admin-console.md) covers claiming the admin credential on that first connection.

### Write something of your own

The smallest thing worth building is one persistent object, one operation on it, and one reaction that fires with the state change. [`first-hour.md`](first-hour.md) builds exactly that and no more, ending with a `reboot` that proves the object, its state, and its references to other objects all survived a real process exit. Then [`first-application.md`](first-application.md) does it properly, with a domain initd and a daemon, an atomicity demonstration whose failure leaves nothing behind, and a hot fix compiled into the running image; [`first-http-endpoint.md`](first-http-endpoint.md) puts it on the wire. The finished state of both is committed at [`../examples/kv-tutorial`](../examples/kv-tutorial), so you can diff your work against it.

The documented command and expected-output pairs in those three documents are replayed against a live boot by [`../scripts/README.md`](../scripts/README.md) tutorial-smoke.sh, parsed out of the documents themselves at run time, so what they tell you to type is checked against what the platform currently does.

**If your use case is agent identity specifically**, note what week 1 does not include. The agent substrate -- agent principals, delegation, token ceremonies, suspension -- is reference-depth material today ([`identity.md`](identity.md), [`system-daemons.md`](system-daemons.md), and the runnable [`../examples/agent-app`](../examples/agent-app), which needs the crypto module for every phase). There is no tutorial-scale entry to it. Plan the substrate as part of stage 2 rather than expecting week 1 to introduce it.

### Run the proofs

[`runtime-primitives.md`](runtime-primitives.md) states, per primitive, the mechanism behind it and a one-command proof. Five distinct commands cover all eight:

| Command | Primitives whose proof it runs |
|---|---|
| `scripts/run-example.sh atomic-demo` | Atomicity |
| `scripts/run-example.sh merry-app` | Persistent state, capability separation, sandboxed code load, asynchronous events |
| `scripts/run-example.sh hot-reload-master` | Hot reload |
| `scripts/run-example.sh chat-app` | Multi-agent coherence |
| `scripts/drive-verbs-smoke.sh` | State introspection |

Each takes `DGD_BIN=/path/to/dgd/bin/dgd` and needs no crypto module. Each ends in `PASS` with its sentinel count, and [`../scripts/README.md`](../scripts/README.md) names the count to expect for every profile. All five together ran in under a minute on the hardware this page was checked against, which is a datum rather than a guarantee.

Read the result precisely: these commands run the proofs the At a glance table names, which is not the same as the primitives being equally proven. That table marks each primitive **Validated** or **Partial**, and the split is deliberate -- the persistence core is validated, while several others have their foundation present with demonstration incomplete. The table is authoritative on which is which; a passing run does not move a Partial to Validated.

For the whole surface rather than the eight proofs, [`../scripts/README.md`](../scripts/README.md) Full regression sweep runs every example and smoke end to end in about fifteen minutes, with the crypto-gated steps reporting documented skips on a module-less build.

### Finishing week 1

You are done when the five commands pass on your hardware and you have your own object surviving a restart. If the proofs pass but the tutorials felt slow, that is data for week 12: the language ramp is the risk [`evaluating.md`](evaluating.md) prices first.

## Weeks 2-3: translate one workflow

### Choosing the workflow

Pick the smallest workflow that exhibits the pain the platform's primitives remove, not the most valuable one. Value makes a trial political; size makes it finish. The symptoms worth looking for, and what replaces each:

| Symptom in your current system | What the platform replaces it with | Where to read it |
|---|---|---|
| You keep a cache and a store in sync by hand, or write serialization code for state that is really just live objects | Orthogonal persistence: objects survive restart with no save/load layer | [`persistence.md`](persistence.md) |
| You wrote compensating logic to undo the half-finished effects of a failed operation | Atomic functions: the runtime rolls back every mutation on error | [`first-application.md`](first-application.md), [`../examples/atomic-demo`](../examples/atomic-demo) |
| The same authorization check is repeated at every entry point, and you are not certain it is repeated everywhere | Capability separation with a single choke point | [`capability.md`](capability.md) |
| A state change must fan out to bookkeeping -- audit, index, notification -- that must never diverge from it | Observers firing synchronously inside the same commit | [`observers.md`](observers.md), [`../examples/signal-app`](../examples/signal-app) |
| Shipping a fix means draining connections and restarting | Hot reload into the live image | [`changing-a-running-system.md`](changing-a-running-system.md) |
| Several actors need a consistent view and you reach for locks, transactions, or a coordination service | One coherence domain, one task at a time | [`execution-model.md`](execution-model.md) |

Two or more symptoms in one workflow is a good candidate. One symptom is usually not worth a platform trial.

Then check the candidate against [`evaluating.md`](evaluating.md) Fit and anti-fit a second time, at workflow scope rather than system scope. A workload that fits the platform overall can still contain one stage that does not, and it is better to find that in week 2.

### Where the translation is direct

Request handling, entity modeling, business rules, and authorization checks port with their structure intact. [`application-authoring.md`](application-authoring.md) is the reference for the shapes, [`common-tasks.md`](common-tasks.md) carries recipes for the recurring pieces, and [`first-composition.md`](first-composition.md) grows the tutorial store into something with capability gating, an audit observer, and a tagged write that commits entry, index, and event together -- the nearest thing in the tree to a realistic application built in stages.

### Where it needs rethinking

Section by section, these are the four that reliably require a design change rather than a translation. This is where the platform's value tends to show up, and also where a trial stalls if the team meets them by surprise.

- **Queries.** There is no query planner over the image. Cross-entity lookup is enumeration plus indexes your write sites maintain ([`application-authoring.md`](application-authoring.md) Modeling domain data; `first-composition.md` builds exactly such an index). If your workflow leans on ad-hoc queries, this is the largest rewrite in the port.
- **Parallelism.** Exactly one task runs in the image at a time, to completion, under a per-task tick budget. Added concurrency buys queueing, not parallelism, and compute-heavy stages belong in a client at the transport boundary ([`execution-model.md`](execution-model.md#run-to-completion) Run to completion).
- **Durability.** Persistence is periodic statedump, not per-transaction commit: an unclean stop loses everything since the last completed dump, including writes already acknowledged to clients. Sizing `dump_interval` is the decision ([`operations.md`](operations.md#availability-and-data-loss-model) Availability and data-loss model); making one specific write durable at acknowledge time is a worked recipe with a trap in it ([`common-tasks.md`](common-tasks.md#make-one-write-durable-at-acknowledge-time) Make one write durable at acknowledge time).
- **Dependencies.** There is no package manager and no third-party corpus. What a service stack solves by adding a dependency, you write and own; external software integrates at the transport boundary as a client or server, never as an import ([`evaluating.md`](evaluating.md) Adoption risks, priced).

One choice here reaches all the way to week 12: how you represent state determines what you can export later. Three representations export to portable form today -- `save_object` text, Vault-and-Schema XML, and the property-table marshal -- while a typed object graph with neither schema nor property-table shape has no export walker ([`persistence.md`](persistence.md#getting-data-out) Getting data out). Deciding this in week 2 with the exit in view costs nothing; discovering it in week 11 costs a walker you have to write before you can leave.

### Where your code lives

Your application is not a fork of this repository. [`application-repository.md`](application-repository.md) covers the split, a recommended layout, and how a checkout composes with the platform tree; its Assembling a production application is a dependency-ordered checklist that sequences the recipes into something deployable.

### Finishing weeks 2-3

You are done when the workflow runs on the platform and you can list, from your own experience, which parts were direct and which needed rethinking.

Keep that list. No worked port exists in this repository -- every example is a greenfield build -- so you are doing this without a reference translation, and your list is the artifact the project most lacks. [Either way, tell the project](#either-way-tell-the-project) is where it goes.

## Weeks 4-6: run it in parallel

Operate both versions against the same inputs and measure. This stage is the least served by the platform today, and the honest framing is that you will build much of the instrumentation yourself.

### What the platform measures, and what it does not

[`../scripts/README.md`](../scripts/README.md) measure-baseline.py is the timing rig: it boots cold timed to console-ready, grows the image in steps, records the client-observed snapshot pause and snapshot size at each step, times a restore boot, and drives sequential requests against a deployed HTTP application. It characterizes *the platform's* envelope, and the numbers it produced -- every one of them measured against the bundled example -- are in [`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity.

`--app-dir` points the same rig at your own domain: it deploys that directory as a `src/usr` domain and drives the sequential and `--concurrent` shapes against your route, with `--app-health` naming the route that signals readiness, `--app-route` the route to drive, and `--app-module` any loadable module your domain needs at boot.

Three limits are worth knowing before you plan around it. Your application answers on the `WWW` mount and no other, because the platform's HTTP bootstrap clones the application at the kernel-defined path `/usr/WWW/obj/server`; deploy it under another name and the driver boots cleanly, reports console-ready, and then never answers the health route. `--app-mount` exists for a domain that is not the HTTP entry point. The load the rig drives is unauthenticated GETs, so a workload sitting behind a bearer token or shaped by a request body still needs a driver of your own. And `--tls`, `--headline`, and `--state-workload` keep their own bundled examples and refuse `--app-dir`, because each measures fixed platform machinery rather than your code.

### Building your own comparison

- **Latency and throughput.** On the platform side, `measure-baseline.py --app-dir` covers the unauthenticated GET shape against your own routes (What the platform measures, and what it does not above); anything authenticated or body-shaped, and everything on the system you are comparing against, is still your own driver with the same inputs on both. The platform's published figures are a reference point for what the shape should look like, not a substitute for measuring your workload ([`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity).
- **Correctness.** Run both against the same inputs and compare outputs. On the platform side, assertions can ride the same boot-time sentinel-driver pattern the examples use, which is also how you get a regression suite out of the exercise rather than a throwaway ([`../scripts/README.md`](../scripts/README.md) Adding a regression).
- **The dump pause.** Whatever `dump_interval` you chose in stage 2 has a recurring cost at your image size, visible from the client side. This is the one platform-specific measurement with no equivalent on your current stack, and it is the one most likely to surprise an operator ([`operations.md`](operations.md#availability-and-data-loss-model) Availability and data-loss model).
- **Operational signal.** Stand up the health route and the counts a monitor would read, so the parallel run produces operational data and not just benchmark numbers ([`operations.md`](operations.md#monitoring-signals) Monitoring signals; [`common-tasks.md`](common-tasks.md#expose-a-health-check-for-monitoring) Expose a health check for monitoring).

### Cost

This documentation set has no cost model, and no published comparison backs a cost claim. What you can price concretely is the platform side: one process on one machine, sized against the ceilings in [`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity, against whatever your current stack bills for the components the platform replaces. That comparison is yours to make, and if you make it, it is among the most useful things you could send back.

### Two things to plan around

- **The connection ceiling.** A stock driver build caps concurrent connections, and the cap counts concurrent streams rather than registered users. What holds a slot, and what a pooling proxy does and does not buy, is [`operations.md`](operations.md#connection-slot-economics) Connection-slot economics. Size the parallel run against it rather than discovering it under load.
- **Endurance is unmeasured.** [`evaluating.md`](evaluating.md) The measured envelope says so directly: sustained behavior near the ceilings remains uncharacterized. A weeks-4-6 parallel run is a sustained run, so you may be the first to characterize it. If you record memory, snapshot pause, and swap use over a multi-hour run, that trajectory is data the project does not have.

### Finishing weeks 4-6

You are done when you have measurements from both systems on the same inputs, and a written account of where the runtime delivered and where it did not.

## Weeks 7-12: decide

Three outcomes. Migrate the workflow, expand to others, or step back.

### What the measurements should settle

[`evaluating.md`](evaluating.md) Adoption risks, priced lists the risks a priori. The point of the parallel run is to replace estimates with your own answers:

- Did the language ramp cost what you budgeted, and can a second team member pick it up from the same materials?
- Did the absent tooling slow the team materially? There is no language server, no step debugger, and no formatter; the console's introspection verbs stand in for a debugger ([`debugging-applications.md`](debugging-applications.md#the-working-environment-plainly) The working environment, plainly).
- Did the durability model fit, or did `dump_interval` sizing turn into a running argument with the business?
- Did you reach a ceiling, at what workload, and was it one with configuration headroom or a compiled bound?
- Did anything need an outbound connection to an external service? That surface ships but is proven in one direction only, and its interaction with an enclosing atomic function is untested ([`http-applications.md`](http-applications.md#outbound-connections) Outbound connections).

### If you migrate

[`operations.md`](operations.md#day-0-standing-up-a-production-deployment) Day 0 is the ordered sequence for a first production deployment, and three of its orderings are load-bearing rather than stylistic. Beyond that, migration commits you to things a trial does not: a backup schedule with a rehearsed restore rather than a backup alone ([`operations.md`](operations.md#backing-up-and-restoring-state) Backing up and restoring state), an LPC-literate team member who stays, a supervisor owning restarts, and a stated position on two dependencies -- the AGPL driver with its single primary maintainer ([`architecture.md`](architecture.md#the-driver-dependency) The driver dependency) and this kernel layer with its own ([`../CONTRIBUTING.md`](../CONTRIBUTING.md) Maintenance). [`changing-a-running-system.md`](changing-a-running-system.md) is day two.

### If you step back

The cost is the export, and it depends on the representation decision made back in stage 2. [`persistence.md`](persistence.md#getting-data-out) Getting data out names what each representation can export and what has no walker. If you shaped state with the exit in view, stepping back is a data migration; if you did not, write the walker before you need it under time pressure.

Stepping back is not a failed trial. A team that ran the parallel and left with numbers knows more about its own workload than it did in week 1, and the project learns where the surface is thin.

### Either way, tell the project

The channels that exist, and what to expect from them, stated plainly rather than promised:

- **Issues** on this repository, using the template that matches the shape ([`../README.md`](../README.md) Community). Questions have their own template.
- **Security reports** go privately through [`../SECURITY.md`](../SECURITY.md), never through an issue.
- **What to expect** is a fact to price, not a service level: the project has a single primary maintainer, review of external contributions is best-effort with no committed turnaround, and CI reruns the module-less regression bar on every pull request ([`../CONTRIBUTING.md`](../CONTRIBUTING.md) Maintenance). If maintenance pauses, the license permits carrying a fork without negotiation and [`../scripts/README.md`](../scripts/README.md) defines what such a fork must keep passing.

What is most useful to send, in rough order: the rethinking list from stage 2, any place a document told you something that turned out not to hold, the endurance trajectory if you captured one, and the cost comparison if you made one. The first two need no permission and cost you nothing but the writing.

## Where to next

- [`evaluating.md`](evaluating.md) -- the fit decision this page assumes, and the depth reading ordered by remaining budget.
- [`getting-started.md`](getting-started.md) -- week 1's first command.
- [`coming-from-contemporary-infrastructure.md`](coming-from-contemporary-infrastructure.md) -- the mechanism-by-mechanism translation from a cloud-service stack, if that is where you are arriving from.
- [`application-repository.md`](application-repository.md) -- where your code lives beside the platform tree, from stage 2 onward.
- [`runtime-platform-roadmap.md`](runtime-platform-roadmap.md) -- what ships today versus what is committed on an activation trigger, for a decision that depends on something not yet here.
