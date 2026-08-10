# Building

Building eOS-kernellib means building DGD. The kernel layer is LPC source that DGD compiles at runtime. The build work in this guide is for the [DGD] driver, which is a build-time dependency.

**Audience**: a developer building DGD as a dependency of eOS-kernellib, comfortable with a C toolchain (`cc`, `make`, `bison` or `yacc`).

## DGD

DGD is the LPC runtime that loads and executes the kernel layer. eOS-kernellib targets DGD 1.7.x, from upstream `master` at or after `975e927f` (`preprocess_file()`, 2026-07-12) -- the 1.7.9 release predates that kfun and fails to boot the kernel layer.

### Standard build

```sh
git clone https://github.com/dworkin/dgd.git
cd dgd
git checkout 975e927f    # 1.7.9 + preprocess_file(); the kernel layer requires this kfun
cd src
make install
```

The driver binary lands at `dgd/bin/dgd`. Run `make clean` from `dgd/src` to start over if a build dirties the tree.

Verify the driver built by running it without arguments from the `dgd` repository root (the build installs to the repo root's `bin/`, not `src/bin/`). It prints a usage line.

```sh
cd ..        # back to the dgd repository root
./bin/dgd
```

### macOS Command Line Tools

If macOS is the build host and only the Xcode Command Line Tools are installed (no full Xcode), `make` fails on `yacc` because `/usr/bin/yacc` is an Xcode-select stub that exits unless Xcode is present. Invoke `bison` at full path explicitly:

```sh
make YACC="/Library/Developer/CommandLineTools/usr/bin/bison -y" install
```

The Command Line Tools' `bison` binary at the path above (GNU Bison 2.3) runs correctly when invoked directly.

### Linux

The Standard build works as written. Validated on Debian 12 (bookworm, aarch64) 2026-07-30: the toolchain is `gcc`, `g++`, `make`, `bison`, and `git` (Debian's `bison` package provides the `yacc` the Makefile invokes -- no workaround needed), and `scripts/run-example.sh` additionally needs `pgrep` from `procps`. The committed [`Dockerfile`](../Dockerfile) at the repository root is the executable form of these notes: it builds the pinned commit from this package set (plus `ca-certificates` for the HTTPS clone) and runs the example regressions to their PASS sentinel ([`docs/getting-started.md`](getting-started.md#run-it-in-a-container) Run it in a container).

### Other platforms

The DGD source compiles on FreeBSD and other POSIX-compatible systems with a working C toolchain. Platform detection happens via `uname -s` at the top of `dgd/src/Makefile`. On Windows, no native build is validated: the supported route is the Linux path -- the container recipe above, or WSL2.

### Wider index types: unproven today

The stock build's capacity ceilings trace to compile-time type widths: `uindex`, `Sector`, and `ssizet` default to `unsigned short` (`src/config.h` in the DGD source), which is why `swap_size` caps at 65535 sectors and swap capacity scales only through `sector_size` (`docs/configuration.md` Limits and capacity). The driver's Makefile exposes a `DEFINES` hook and `config.h` takes `UINDEX_TYPE` / `SECTOR_TYPE` / `SSIZET_TYPE` overrides.

Stated as observed, not promised: a naive widening (all three types to `unsigned int`, 2026-07-12, macOS arm64) compiles cleanly and segfaults at cold boot before the first banner line. A working wider-index build is therefore a driver-level task with upstream guidance, not a flip of these defines; until one exists, the stock-snapshot-compatibility question and the wider-index memory cost stay unmeasured (`docs/configuration.md` Unmeasured today).

## eOS-kernellib

There is no build step. DGD compiles the LPC source under `src/` at runtime, on first load and on hot-reload requests.

Verify the kernel layer compiles by running the driver against `example.dgd` per the steps in `docs/getting-started.md`. Compile errors surface in the driver's standard output during boot.

## State and snapshot files

`example.dgd` references swap and snapshot files under `../state/`. The `state/` directory ships with the checkout (it holds a tracked `.gitignore`). If you point those settings elsewhere, create the directory they name before starting the driver. The swap file is recreated on each boot. The snapshot file persists across boots and stores the runtime's object graph as of the last dump taken (snapshots are always deliberately triggered -- `docs/persistence.md` The statedump cycle).

## Where to next

- [`docs/getting-started.md`](getting-started.md): run the example configuration once the driver is built.
- [`docs/configuration.md`](configuration.md): the `.dgd` configuration field reference and capacity ceilings.
- [`docs/operations.md`](operations.md): boot modes and the operator surface.
- [`docs/architecture.md`](architecture.md): the platform's tier model and where the build fits.

[DGD]: https://github.com/dworkin/dgd
