# Your first contribution

A hands-on walkthrough of the contributor loop. [CONTRIBUTING.md](../CONTRIBUTING.md)'s Anatomy of a mergeable change reads two merged PRs as templates and names three starter changes sized for a first PR; this document executes that loop, command by command, on the smallest of them: an additional dispatcher trace site, the change whose exact template is PR #31. You will find the change named as an open edge in the docs, reproduce the gap on a live console, write the regression before the code, watch it fail, make a four-line edit, compile it into the running system without a restart, watch the regression pass, run the sweep steps the change touches, and draft the commit. Every command is shown with the output it produced.

**Audience**: a contributor making their first kernel-layer change -- platform built and bootable per [getting-started.md](getting-started.md), comfortable with the console from [first-hour.md](first-hour.md), CONTRIBUTING.md read. The graded tutorials teach application authoring on top of the platform; this walkthrough is the sibling path into the platform's own source and harness.

**The edit here is an exercise.** This walkthrough teaches the loop, so it ends at the commit-message draft and then reverts. Your own first PR carries the same shape through to the end: the code, the regression, the documentation updates the change makes stale, and the signed commit, as one reviewed unit.

## 1. Find the change in the docs

Starter changes are named where the contract already is. [dispatcher.md](dispatcher.md)'s `set_dispatch_trace` entry states the current coverage and lists what is missing:

> Current trace coverage is the `dispatch_set` entry site only. Additional trace sites (batch-entry/exit, observer-fire, cascade-depth-increment, cycle-chain mutation, observer-cache hit/miss) are future-work. The flag-gating contract is established; site additions are mechanical.

The first listed untraced site -- batch-entry/exit -- is the change. Before touching anything, read the template. PR #31 established the flag-gating contract this change extends; from a clone, without GitHub:

```text
$ git log --oneline --grep='#31'
58dd69e Merge pull request #31 from eOSContinuum/feature/logd-consumer-adoption
$ git show -m 58dd69e --stat
```

Its diff shows the shape you are about to reproduce at a new site: the `_trace_dispatch` emission in `src/usr/Merry/sys/merry.c`, the verbset block in `scripts/verbsets/dispatcher-verbs.verbset` that proves the line arrives end to end, and the documentation files that owned statements the change made stale.

## 2. Boot a platform you can drive

The dispatcher needs a property-bearing object to dispatch against. The harness's own pattern ([drive-verbs-smoke.sh](../scripts/drive-verbs-smoke.sh)) deploys the vault-app example as the `MyApp` domain, whose boot driver creates the named clone `MyApp:core:item1` -- and vault-app does not self-exit, so the console stays up. From the repository root, with a clean slate:

```sh
cp -R examples/vault-app src/usr/MyApp
sed "s|^directory.*|directory = \"$PWD/src\";|" example.dgd > state/run.dgd
$DGD_BIN state/run.dgd > state/boot.log 2>&1 &
```

When the boot log settles (the vault round-trip test prints its Schema cross-check notices), connect to the telnet port and claim the `admin` console:

```text
$ telnet 127.0.0.1 8023
eOS-kernellib runtime platform.
...
login: admin
Pick a new password:
Retype new password:
Password changed.
#
```

One note on the transcripts in this document: they were captured on an instance whose generated config shifts the ports (another instance held the defaults on the same machine), so a port number in a transcript is whatever `state/run.dgd` says. The stock `example.dgd` numbers are 8023 (telnet) and 8080 (binary); every command below reads the port from the generated config, and the flow is identical.

## 3. Reproduce the gap

The trace contract established by PR #31: `dispatch-trace on` enables emission, trace lines land in the general `logd` stream at DEBUG, and `logd`'s threshold must admit DEBUG for them to reach the sink -- so pair the flag with `log-level debug` and read the result with `log` ([operations.md](operations.md) Logging and diagnostics). Turn both knobs, drive one property write across the dispatcher, and read the log:

```text
# dispatch-trace on
dispatch-trace on
note: trace lines emit at DEBUG and the current log-level suppresses them; `log-level debug` to see them
# log-level debug
log-level set to DEBUG
# code "/usr/Index/sys/index_daemon"->query_object("MyApp:core:item1")->set_property("walk:demo:prop", 1)
$0 = 1
# log
...
Jul 29 21:54:19 DEBUG MERRY trace: dispatch_set /usr/MyApp/obj/item#246:walk:demo:prop
#
```

One trace line: the `dispatch_set` entry site. Nothing else -- yet [dispatcher.md](dispatcher.md)'s implicit-batch semantics say this unbatched write entered a fresh implicit batch, allocated a batch-id, and exited it. The batch boundary crossed twice and left no trace. That is the gap, observed.

## 4. Write the regression first

The common skeleton in CONTRIBUTING.md pairs every change with the regression that demonstrates it, and writing the assertion first gives you a red-then-green proof that the test actually tests. The dispatcher's console regressions live in [scripts/verbsets/dispatcher-verbs.verbset](../scripts/verbsets/dispatcher-verbs.verbset), blocks of `cmd:` / `expect:` / `absent:` lines driven over a live telnet console by `scripts/drive-verbs.py` (the file format is documented in that script's docstring). PR #31's TRACE VISIBILITY block is the template; append a sibling block asserting the lines that do not exist yet:

```text
# BATCH TRACE -- the batch-entry/exit trace site: with the flag on and
# the threshold lowered, a single unbatched write shows its implicit
# batch boundary (entry with the allocated batch-id and atomic flag,
# exit with the same id) around the dispatch_set entry line. Both
# knobs are restored afterward.

cmd: dispatch-trace on
expect: dispatch-trace on
expect: note: trace lines emit at DEBUG

cmd: log-level debug
expect: log-level set to DEBUG

cmd: code "/usr/Index/sys/index_daemon"->query_object("MyApp:core:item1")->set_property("drive:bt:prop", 1)
absent: usage:

cmd: log
expect: MERRY trace: batch-entry id=\d+ atomic=0
expect: MERRY trace: batch-exit id=\d+

cmd: log-level info
expect: log-level set to INFO

cmd: dispatch-trace off
expect: dispatch-trace off
```

Now watch it fail. The harness form boots its own cold platform, so stop your boot first (the script refuses to run beside another instance and tells you so):

```text
$ DGD_BIN=<dgd> DEPLOY=vault-app:MyApp scripts/drive-verbs-smoke.sh scripts/verbsets/dispatcher-verbs.verbset
== clean slate (base boot) ==
== deploy vault-app as the MyApp domain ==
== boot ==
== drive scripts/verbsets/dispatcher-verbs.verbset ==
...
FAIL [42] log
        expect failed: /MERRY trace: batch-entry id=\d+ atomic=0/
        expect failed: /MERRY trace: batch-exit id=\d+/
...
== 43/44 verbs PASS ==
DRIVE-VERBS FAIL
```

Exactly one failing block, and it is yours: the log shows the `dispatch_set` lines and nothing at the batch boundary. Red, for the right reason. (While your own boot is up you can get the same red without a cold boot: `python3 scripts/drive-verbs.py scripts/verbsets/dispatcher-verbs.verbset --port <port>` drives the verbset against the live console.)

## 5. Make the edit

The dispatcher lives in `src/usr/Merry/sys/merry.c`. Three facts locate the change:

- `_trace_dispatch(string msg)` is the established emitter: flag-gated, routes to `logd` at DEBUG, elides everything when the flag is off. The new site calls it; nothing about emission changes.
- All batch boundaries -- explicit `batch()` and `batched_set()` frames and the implicit batch wrapped around an unbatched write -- funnel through one helper pair, `_push_batch_context` / `_pop_batch_context`. Two call sites cover every batch.
- The helpers sit above `_trace_dispatch` in the file, and LPC resolves calls against declarations already seen, so the edit carries a forward declaration after the inherit block (the same pattern `src/usr/System/sys/identityd.c` uses for its private helpers).

The whole diff:

```diff
--- a/src/usr/Merry/sys/merry.c
+++ b/src/usr/Merry/sys/merry.c
@@ -24,6 +24,8 @@ inherit "/usr/Merry/lib/merryapi";
 private inherit "/lib/util/lpc";
 inherit "/lib/util/named";
 
+private void _trace_dispatch(string msg);
+
 # define NODE_COUNT	256
 
 # define HALFLIFE	30
@@ -639,6 +641,8 @@ int _push_batch_context(int atomic_mode, mapping opts) {
    batch_id = next_batch_id ++;
    stack += ({ _make_batch_context(batch_id, atomic_mode, opts) });
    tls_set(TLS_BATCH_STACK, stack);
+   _trace_dispatch("batch-entry id=" + (string) batch_id +
+		   " atomic=" + (string) atomic_mode);
    return batch_id;
 }
 
@@ -652,6 +656,7 @@ void _pop_batch_context() {
    if (n == 0) {
       error("MERRY: _pop_batch_context called with empty batch stack");
    }
+   _trace_dispatch("batch-exit id=" + (string) stack[n - 1]["batch_id"]);
    if (n == 1) {
       tls_set(TLS_BATCH_STACK, nil);
    } else {
       tls_set(TLS_BATCH_STACK, stack[.. n - 2]);
```

Mechanical, as dispatcher.md promised: the site names itself in the message, the flag-gating, routing, and threshold behavior all belong to `_trace_dispatch` already. Match the file's style rather than your own ([CONTRIBUTING.md](../CONTRIBUTING.md) Code style): the message-building shape and `(string)` casts follow PR #31's emission, the indentation is the file's tab profile.

## 6. Compile it into the running system

No restart. The kernel-layer live-change matrix ([changing-a-running-system.md](changing-a-running-system.md) Changing the kernel layer) says a System-tier daemon recompiles in place with `compile <path>`: master variables survive, `create()` does not re-run. The Merry daemon is that case -- its script caches, registrar tables, and the `dispatch_trace` flag all ride through. Boot your instance again if you stopped it for the harness run, edit the source on disk, then from the console:

```text
# compile /usr/Merry/sys/merry.c
$0 = </usr/Merry/sys/merry>
# dispatch-trace on
dispatch-trace on
note: trace lines emit at DEBUG and the current log-level suppresses them; `log-level debug` to see them
# log-level debug
log-level set to DEBUG
# code "/usr/Index/sys/index_daemon"->query_object("MyApp:core:item1")->set_property("walk:demo:prop", 2)
$1 = 2
# log
...
Jul 29 21:55:35 DEBUG MERRY trace: dispatch_set /usr/MyApp/obj/item#246:walk:demo:prop
Jul 29 21:55:35 DEBUG MERRY trace: batch-entry id=21 atomic=0
Jul 29 21:55:35 DEBUG MERRY trace: batch-exit id=21
# log-level info
log-level set to INFO
# dispatch-trace off
dispatch-trace off
```

The same write that left one line in section 3 now leaves three: the batch boundary is visible, with the allocated id and the non-atomic flag. (The `dispatch_set` line precedes `batch-entry` because the dispatcher logs its entry before it allocates the implicit batch -- worth knowing before you assert ordering in a test.) The gap you observed and the fix you compiled are on the same boot, minutes apart; that is the platform's own change story doing the contributing.

## 7. Green under the harness

The live proof is not the reviewable proof. Stop your boot and run the harness form again -- cold boot, clean slate, the exact run a reviewer reproduces:

```text
$ DGD_BIN=<dgd> DEPLOY=vault-app:MyApp scripts/drive-verbs-smoke.sh scripts/verbsets/dispatcher-verbs.verbset
...
PASS [41] code "/usr/Index/sys/index_daemon"->query_object("MyApp:core:item1")->set_property("drive:bt:prop", 1)
PASS [42] log
PASS [43] log-level info
PASS [44] dispatch-trace off
== 44/44 verbs PASS ==
DRIVE-VERBS PASS
```

Red before the edit, green after, same command: the regression demonstrably tests the change.

## 8. Run the sweep steps the change touches

The pre-PR bar is the Full regression sweep ([scripts/README.md](../scripts/README.md)); [full-sweep.sh](../scripts/full-sweep.sh) runs it end to end and takes step subsets for iterating. This change touches the drive-verbs steps (15 and 16 -- the default verbset run that includes dispatcher-verbs, and the module-less console regressions) plus the generated-index check (29, since a source edit can add a callable the index must carry):

```text
$ DGD_BIN=<dgd> scripts/full-sweep.sh 15 16 29
```

Observed on this checkout: step 15's boot drives all ten default verbsets with dispatcher-verbs at 44/44; step 16's four console verbsets all report `DRIVE-VERBS PASS`; step 29 prints `function-index.md up to date` (the new calls are to an existing private helper, so the index owes nothing). One environment note from the shifted-port capture: `port-labels.verbset` asserts the label-to-port wiring structurally (label, a numeric port, manager), so it passes whichever ports the boot's config chose. Your PR's verification section quotes these commands and their pass signals; CI reruns the module-less bar for you, and the module-bearing steps stay yours to evidence ([CONTRIBUTING.md](../CONTRIBUTING.md) Pull request flow).

## 9. Find the docs the change owes

A change owes an update to every doc that states the behavior it changed; find them by grepping `docs/` for the daemon, verb, and file names you touched:

```sh
grep -rln 'dispatch-trace\|dispatch_trace\|_trace_dispatch' docs/
```

For this change the hits that own now-stale statements:

- **[dispatcher.md](dispatcher.md)** -- the `set_dispatch_trace` entry states "Current trace coverage is the `dispatch_set` entry site only" and lists batch-entry/exit as future-work. Both statements are what this change falsifies; the entry gains the new site and the future-work list loses it.
- **[operations.md](operations.md)** -- the diagnostics-routing paragraph states "the current scope emits one trace line per `dispatch_set` entry (object name + path). Additional trace sites are future-work."
- **[admin-console.md](admin-console.md)** -- the Tracing bullet describes `dispatch-trace on` as "per-`dispatch_set` entry logging", and the verb appendix row reads "verbose dispatch-entry tracing".
- **`merry.c` itself** -- the `dispatch_trace` state comment and `_trace_dispatch`'s own comment both name batch-entry as a future site; the code change updates its adjacent prose in the same commit.

This walkthrough stops at the finding -- the exercise is not merged, so the docs stay true as written. Your real PR makes these edits, and they are not padding: PR #31 shipped five documentation updates for exactly this reason.

## 10. Draft the commit

Per [CONTRIBUTING.md](../CONTRIBUTING.md) Commit conventions -- atomic, signed (`git commit -S -s`), imperative title of 72 characters or fewer, body leading with outcomes then enumerating per file:

```text
Add batch-entry/exit dispatcher trace site

Verbose dispatcher tracing now shows batch boundaries: with
dispatch-trace on, every batch-context push emits a batch-entry line
carrying the allocated batch-id and atomic flag, and every pop emits
the matching batch-exit line, to the general logd stream at DEBUG
level. One helper pair covers every boundary -- explicit batch() and
batched_set() frames and the implicit batch around an unbatched
write all route through the shared push/pop.

- src/usr/Merry/sys/merry.c: _trace_dispatch calls added in
  _push_batch_context and _pop_batch_context, with a forward
  declaration ahead of the batching surface; the trace-coverage
  comments name the new site.
- scripts/verbsets/dispatcher-verbs.verbset: BATCH TRACE block
  drives a write with the flag on and asserts the batch-entry and
  batch-exit lines arrive via the log verb.
- docs/dispatcher.md: set_dispatch_trace coverage statement extended;
  batch-entry/exit removed from the future-work list.
- docs/operations.md: trace-scope sentence updated to include batch
  boundaries.
- docs/admin-console.md: Tracing bullet and verb appendix row
  updated.
```

That message describes the full mergeable unit -- code, regression, and the documentation from section 9 -- landing as one reviewed PR against `main`, on a branch named for the shape (`feature/batch-trace-site`), with the section 8 commands and pass signals in the PR's verification section.

## What you just used

The loop, named: the docs as the source of open edges (a starter change is a documented contract with a listed gap), a merged PR as the template (`git show -m` on the merge commit), the console as the reproduction surface, the verbset harness as the regression form, red-then-green as the proof the test tests, live recompile as the iteration loop, the step-subset sweep as the pre-PR bar, `grep docs/` as the documentation-debt detector, and the commit conventions as the delivery format. Every piece generalizes: the other two starter changes in CONTRIBUTING.md (`cascade-aborted` test phase, chat transport binding) and any change of your own walk the same sequence with different files.

## Where to next

- **[../CONTRIBUTING.md](../CONTRIBUTING.md)** -- the full contribution reference this walkthrough executed one pass of: How to propose a change (open the issue first), Code style at line level, Pull request flow, and the other two starter changes.
- **[../scripts/README.md](../scripts/README.md)** -- the Full regression sweep, annotated step by step: what each of the 34 steps proves and its pass signal. Section 8 ran the subset; the full bar is the pre-PR run.
- **[dispatcher.md](dispatcher.md)** -- the subsystem this walkthrough touched, at reference depth: registration, timings, batching, bounds, and the verification table that maps each contract to the evidence that exercises it.
- **[where-code-belongs.md](where-code-belongs.md)** and **[source-map.md](source-map.md)** -- placement doctrine and the subsystem-to-doc map, for finding where your own change belongs and which doc it will owe.
