<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Getting Started

This guide installs the [DGD] driver, fetches this repository, and runs an eOS-kernellib server with the bundled example configuration.

**Audience**: a new user setting up DGD and eOS-kernellib for the first time; comfortable with the shell; has not yet booted the substrate.

## Prerequisites

A POSIX-compatible system with a C compiler (`cc` or `gcc`), `make`, `bison` (or `yacc`), and `git`.

## Install DGD

Clone the DGD source, build the driver, and install the binary:

```sh
git clone https://github.com/dworkin/dgd.git
cd dgd/src
make install
```

The driver binary lands at `dgd/bin/dgd`. See `doc/building.md` for platform-specific notes.

## Fetch eOS-kernellib

```sh
git clone https://github.com/eOSContinuum/eOS-kernellib.git
cd eOS-kernellib
```

The repository contains LPC source under `src/`. DGD compiles LPC at runtime; the kernel layer has no separate build step.

## Run the example configuration

`example.dgd` is a starter DGD configuration that mounts the kernel layer's source directory and binds the kernel's default ports.

Edit `example.dgd` to set `directory` to the absolute path of `eOS-kernellib/src/`:

```text
directory       = "/absolute/path/to/eOS-kernellib/src";
```

Create the state directory referenced by the `swap_file` and `dump_file` settings:

```sh
mkdir -p state
```

Run the driver against the configuration:

```sh
/path/to/dgd/bin/dgd example.dgd
```

The driver compiles the kernel objects and binds two ports:

- `8023` — telnet port for administrative access
- `8080` — HTTP/1 port

## Connect

Telnet to the administrative port:

```sh
telnet localhost 8023
```

The HTTP/1 port (8080) accepts requests from any HTTP/1 client. Without an application mounted on top, the server returns errors for routes it does not handle.

## Where to next

- `examples/http-app/README.md` and `doc/http-applications.md` cover the HTTP/1 application pattern; the example is the natural next read once the substrate is running.
- `doc/admin-console.md` covers connecting to the telnet port, the first-cold-boot admin-password prompt, and the operator's verb surface.
- `doc/architecture.md` covers the substrate's tier model, daemons, and boot sequence; `doc/substrate-primitives.md` covers the per-primitive foundation-and-proof statement.
- `doc/application-authoring.md` covers writing a tier-E application on top of the substrate (non-HTTP transports, owner/access conventions, the `call_touch` upgrade model).
- `doc/building.md` covers DGD build details and platform-specific notes.

[DGD]: https://github.com/dworkin/dgd
