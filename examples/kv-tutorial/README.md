# kv-tutorial

The finished state of [first-application.md](../../docs/first-application.md),
[first-http-endpoint.md](../../docs/first-http-endpoint.md) and
[first-vault-entity.md](../../docs/first-vault-entity.md): every file those
three tutorials build, after every edit each one applies, exactly as the
prose leaves it. This is not a harness example -- there is no
`scripts/run-example.sh` profile for it, and the harness never touches
this directory, so the finished copies survive every clean-slate run.
(A hand-deployed copy under `src/usr/` is another matter: the example
harness removes `src/usr/WWW` on every `scripts/run-example.sh` run, and
`scripts/tutorial-smoke.sh` -- which the full sweep runs -- removes
`src/usr/KV`, `src/usr/WWW` and `src/usr/Pet` in its own clean-slate
step, so copy anything you want to keep. The third tutorial's Cleaning
up section covers the `WWW` collision from the reader's side.)

## Use it as a diff target

If a hand-typed file from either tutorial will not compile, diff it against
the finished copy here rather than re-reading the prose line by line:

    diff src/usr/KV/sys/kv_daemon.c examples/kv-tutorial/KV/sys/kv_daemon.c
    diff src/usr/KV/sys/kv_vault.c examples/kv-tutorial/KV/sys/kv_vault.c
    diff src/usr/WWW/obj/server.c examples/kv-tutorial/WWW/obj/server.c

## Layout

    KV/initd.c            domain boot: compiles the daemon, the Vault
                            node, and the entry clonable
    KV/sys/kv_daemon.c     the daemon: put/get/remove/size, the atomic
                            rollback demo, and persist/reload/forget
    KV/sys/kv_vault.c      the domain's Vault node: registration and the
                            KV:Entry schema
    KV/obj/entry.c         the entity clonable stored as XML
    WWW/initd.c            domain boot: compiles the server
    WWW/obj/server.c        HTTP/1 server, routed to the KV daemon
                            (GET/PUT/DELETE on /kv/<key>, GET /health)

## Deploying by hand

    cp -R examples/kv-tutorial/KV src/usr/KV
    cp -R examples/kv-tutorial/WWW src/usr/WWW

Then compile each domain's `initd.c` at the console as the tutorials show;
the two-step console form (compile the initd, then compile the object it
would otherwise compile for you) applies here too.
