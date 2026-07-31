# console-ext-app

The smallest application that ships a first-class admin-console
operator verb. It registers `ext-hello` through the kernel registry's
capability-gated `extend()` surface, so the verb dispatches exactly
like a built-in extension verb (`session`, `identity`, ...) instead of
being tunneled through the console's `code` verb.

## The problem this pattern replaces

An application wanting an operator verb used to need a bootstrap seam:
console `code` objects compile as `/usr/admin/_code`, which an
application daemon's same-domain caller gate correctly refuses, so each
application shipped a relay object whose only job was to forward
console calls into its own domain. With the registration surface, the
application registers `(verb, path, method)` once and the console
dispatches to it directly.

## Layout

    initd.c       domain boot: compiles the daemon
    sys/extd.c    the daemon: registration lifecycle + cmd_ext_hello

## Running it

Deploy as the `ConsoleExt` domain (the source assumes that mount) and
drive the regression verbset:

    DGD_BIN=/path/to/dgd DEPLOY="console-ext-app:ConsoleExt" \
        scripts/drive-verbs-smoke.sh scripts/verbsets/console-ext.verbset

The lifecycle, by hand on the admin console:

    console-ext                          # nothing registered yet
    console-ext approve ConsoleExt       # operator grants the capability
    code "/usr/ConsoleExt/sys/extd"->register_verbs()
    ext-hello operator                   # the verb dispatches
    console-ext unapprove ConsoleExt     # revokes and drops the verbs

The boot-time attempt in `create()` runs before any approval exists, so
it records its refusal (`query_boot_attempt`) instead of failing the
deploy; a deployment that seeds the approval before the domain compiles
(or re-runs `register_verbs()` after approving) gets the boot-time
registration the daemon attempts first.
