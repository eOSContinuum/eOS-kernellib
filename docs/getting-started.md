# Getting started

This guide installs the [DGD] driver, fetches this repository, and runs an eOS-kernellib server with the bundled example configuration.

**Audience**: a new user setting up DGD and eOS-kernellib for the first time; comfortable with the shell; has not yet booted the platform.

**Tested against**: DGD `master` at `975e927f` (the 1.7.9 driver plus `preprocess_file()`, which the kernel layer requires; 2026-07-12) on macOS 26.5 (arm64), validated 2026-07-18. Other POSIX-compatible systems should work; the macOS-specific bison workaround is captured in `building.md`.

## Prerequisites

A POSIX-compatible system with a C compiler (`cc` or `gcc`), `make`, `bison` (or `yacc`), and `git`. For the administrative telnet port you also need a line-mode TCP client: `telnet` where available, or `nc` (netcat), which macOS ships (macOS has not shipped `telnet` since 10.13).

## Zero to a passing proof

The fastest signal, from nothing, in one block -- clone and build the driver, clone the kernel layer, and run an example through a real dump-and-restore cycle (the harness generates its own configuration; no config editing is involved):

```sh
git clone https://github.com/dworkin/dgd.git
(cd dgd && git checkout 975e927f && cd src && make install)
git clone https://github.com/eOSContinuum/eOS-kernellib.git
cd eOS-kernellib
DGD_BIN=$PWD/../dgd/bin/dgd scripts/run-example.sh merry-app
```

A passing run takes a few seconds and ends like this (captured 2026-07-12, DGD 1.7.9 on macOS 26.5 arm64):

```text
== clean slate ==
== deploy merry-app as the MerryApp domain ==
== boot 1 (cold; driver dumps + self-exits) ==
== boot 2 (restore from snapshot) ==
== sentinels ==
MerryApp:test: starting
MerryApp:test: ANCESTRY OK
MerryApp:test: SANDBOX OK
[... 24 more sentinels ...]
MerryApp:test: OBSERVER EVICT OK
MerryApp:test: PERSIST VERIFY OK
== 28 " OK" sentinels (expected 28) ==
PASS
```

The two boots bracket a snapshot restore, so the PERSIST sentinels prove application state survived a real process exit. [`scripts/README.md`](../scripts/README.md) documents the harness; `examples/` holds the other runnable examples. The sections below unpack each step, then boot the platform interactively.

## Install DGD

Clone the DGD source, build the driver, and install the binary:

```sh
git clone https://github.com/dworkin/dgd.git
cd dgd
git checkout 975e927f    # 1.7.9 + preprocess_file(); the kernel layer requires this kfun
cd src
make install
```

The driver binary lands at `dgd/bin/dgd`. See `docs/building.md` for platform-specific notes. DGD is an unmodified upstream dependency: the platform builds on the upstream driver as-is. The kernel layer requires `preprocess_file()`, added to upstream `master` on 2026-07-12 and not yet in a tagged release, so build from the pinned commit above (or any later `master`) -- the 1.7.9 release fails at boot with `undefined function ::preprocess_file`. Note that a `master` build's boot banner still prints `DGD 1.7.9`, so the banner does not distinguish the two.

The build is small. From a clean checkout, `make install` (with the macOS bison workaround from `docs/building.md` where it applies) completes in about two seconds of wall time on an Apple M5 Max, and well under a minute on any recent hardware. There is no dependency fetch: the driver needs only a C++ toolchain and a yacc.

## Fetch eOS-kernellib

```sh
git clone https://github.com/eOSContinuum/eOS-kernellib.git
cd eOS-kernellib
```

The repository contains LPC source under `src/`. DGD compiles LPC at runtime; the kernel layer has no separate build step.

## Boot it yourself: the configuration and the ports

The proof above generated its own configuration and tore its boots down; running the platform interactively is where the configuration becomes yours. `example.dgd` is the starter DGD configuration: it mounts the kernel layer's source directory and binds the kernel's default ports.

Edit `example.dgd` to set `directory` to the absolute path of `eOS-kernellib/src/`:

```text
directory       = "/absolute/path/to/eOS-kernellib/src";
```

The `state/` directory referenced by the `swap_file` and `dump_file` settings ships with the checkout (it holds a tracked `.gitignore`); if you relocated those paths in the config, create the directory they point at.

Run the driver against the configuration:

```sh
/path/to/dgd/bin/dgd example.dgd
```

The driver compiles the kernel objects and binds two ports:

- `8023`: telnet port for administrative access
- `8080`: HTTP/1 port

## Connect

Connect a line-mode TCP client to the administrative port:

```sh
telnet localhost 8023    # or: nc localhost 8023
```

The HTTP/1 port (8080) accepts connections from any HTTP/1 client, but with no application mounted on top there is nothing to answer them: the kernel's HTTP server clones an application server at `/usr/WWW/obj/server` per connection, and when that path is absent the connection is dropped without a response (a client like `curl` waits until its own timeout). Mounting an application there (`examples/http-app/README.md` is the walkthrough) is what makes 8080 respond.

## Where to next

- [`docs/first-hour.md`](first-hour.md) is the natural next step: a hands-on hour from this booted platform to the persistence loop (your own objects, state, and reactions surviving a process restart).
- [`docs/coming-from-contemporary-infrastructure.md`](coming-from-contemporary-infrastructure.md) maps the cloud-service stack (database, queue, deploy pipeline, IAM) onto the platform's mechanisms, if that is where you are arriving from.
- [`examples/http-app/README.md`](../examples/http-app/README.md) and [`docs/http-applications.md`](http-applications.md) cover the HTTP/1 application pattern; the example is the natural next read once the platform is running.
- [`docs/admin-console.md`](admin-console.md) covers connecting to the telnet port, the first-cold-boot admin-password prompt, and the operator's verb surface.
- [`docs/architecture.md`](architecture.md) covers the platform's tier model, daemons, and boot sequence; [`docs/runtime-primitives.md`](runtime-primitives.md) covers the per-primitive foundation-and-proof statement.
- [`docs/application-authoring.md`](application-authoring.md) covers writing a tier-E application on top of the platform (non-HTTP transports, owner/access conventions, the `call_touch` upgrade model).
- [`docs/building.md`](building.md) covers DGD build details and platform-specific notes.
- Contributing? [`scripts/README.md`](../scripts/README.md)'s Full regression sweep is the pre-PR bar; its prerequisites list names the optional pieces worth installing early (the lpc-ext crypto module, the vector-generator toolchain).

[DGD]: https://github.com/dworkin/dgd
