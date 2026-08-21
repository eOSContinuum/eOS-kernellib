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

### Wider index types

The stock build's capacity ceilings trace to compile-time type widths: `uindex` and `ssizet` default to `unsigned short` (`src/config.h` in the DGD source), which is why `swap_size` caps at 65535 sectors and swap capacity scales only through `sector_size` (`docs/configuration.md` Limits and capacity). The driver's Makefile exposes a `DEFINES` hook and `config.h` takes `UINDEX_TYPE` / `SECTOR_TYPE` / `SSIZET_TYPE` overrides. `SECTOR_TYPE` and `CINDEX_TYPE` both default to whatever `UINDEX_TYPE` is, so widening that one type carries the sector and call-out indices with it.

Two rebuilds are known to work, and they carry different amounts of evidence here. Both are stated as observed, not promised.

**The validated form** widens the object and sector indices and leaves string length at its default:

```sh
make DEFINES='-DUINDEX_TYPE="unsigned int" -DUINDEX_MAX=UINT_MAX' install
```

On Linux and Solaris, restate the large-file flag the Makefile would otherwise contribute (The `DEFINES` override, below):

```sh
make DEFINES='-DUINDEX_TYPE="unsigned int" -DUINDEX_MAX=UINT_MAX -D_FILE_OFFSET_BITS=64' install
```

Validated 2026-08-10 against driver `25dad1dd` and kernel layer `b5bcde1`, on macOS arm64 and on Debian 12 aarch64: every example profile passes on both platforms at the module-less bar (the crypto-gated steps skip, as they do for any build without the extension module), and `swap_size` accepts values past 65535 where a stock build refuses with `Config error: int value out of range`.

That validation driver is one commit past the `975e927f` pinned in the Standard build above, and the commit between them ("Properly clear the goto list after a function") changes a single line of `src/comp/codegen.cpp`. Nothing the recipe touches differs across the two trees, so it applies to the pinned commit as written. Build the pin unless you have a reason not to: it is what the container recipe and the regression workflow build.

**Upstream's fuller form** additionally widens the maximum string length to 1 MB:

```sh
make DEFINES='-DUINDEX_TYPE="unsigned int" -DUINDEX_MAX=UINT_MAX -DSSIZET_TYPE="unsigned int" -DSSIZET_MAX=1048576' install
```

This is the form DGD's author recommends when strings need to grow as well as indices, and the bounded `SSIZET_MAX` is the point of it (see the naive-widening failure below). It is verified here only to build, cold boot, and accept `swap_size = 200000`, on macOS arm64 alone -- no example sweep, no snapshot-compatibility check, no memory measurement. Prefer the validated form unless the workload needs strings past 64K.

**Why a naive widening fails.** Setting all three types to `unsigned int` compiles cleanly and segfaults at cold boot before the first banner line. The cause is a cast, not the width: `config.h` defines `MAX_STRLEN` as `SSIZET_MAX`, and string creation guards on `len > (LPCint) MAX_STRLEN`, so an `SSIZET_MAX` of `UINT_MAX` becomes -1 in the driver's signed 32-bit `LPCint`, every string allocation takes the error path, and the error path builds its own message through the same allocator until the stack guard fires. Bounding `SSIZET_MAX` to a value that survives the cast is what makes the fuller form work. (`-DLARGENUM` also produces a working build, by widening `LPCint` so the cast no longer overflows, but it enables an unrelated feature and is not the intended route.)

**The `DEFINES` override.** Passing `DEFINES` on the `make` command line replaces the Makefile's variable outright and suppresses the Makefile's own appends to it -- including the `-D_FILE_OFFSET_BITS=64` that `src/Makefile` adds for Linux and Solaris hosts. Writing `+=` on the command line does not change this: a command-line assignment overrides a plain makefile append whichever operator either side uses. That describes the pinned driver and every DGD before `b4da6a96` (2026-08-21), which fixed it by routing the platform appends through a second variable that a command line does not set. On a driver at or past that commit the flag arrives on the compile line as usual and restating it is merely redundant. Restating it is harmless either way, so the second recipe above is the form that works on both.

What restating it buys is confined to 32-bit hosts. On a 64-bit host -- which is every platform validated here -- `off_t` is already 64 bits and the flag changes no type at all: measured on Debian 12 aarch64, its entire effect on the resulting binary is to redirect a couple of glibc entry points to their large-file variants, which are equivalent under LP64. The flag earns its keep on 32-bit hosts, where it is what lifts the 2 GB file-offset ceiling.

**What a wide build costs, and which ceiling it does not lift.** On the same example and machine, the snapshot grew about 5.7% for identical state (613,376 to 648,192 bytes) and resident memory at rest grew about 80 KB. Treat that memory figure as a floor rather than an estimate: the widened types are per-object and per-sector, so the cost scales with the object graph, and it was measured on a small one. Widening `uindex` does not touch the connection ceiling -- `users` is bounded by `EINDEX_MAX`, a different compile-time type -- and on a connection-driven workload that is the ceiling that binds first (`docs/operations.md` Connection-slot economics).

**Migration is one-way.** A stock-written snapshot restores under a wide driver, so moving forward works; a wide-written snapshot offered back to a stock driver refuses cleanly with `Config error: initialization failed` rather than crashing or restoring partially. Plan the rebuild as a forward-only deploy and keep a pre-migration snapshot (`docs/operations.md` Config changes across a restore states what a restore boot accepts, driver build included).

Still unmeasured on a wide build: the crypto-gated surfaces (native TLS and the identity and agent ceremonies were not exercised under one), memory cost at a realistic graph size, and sustained multi-day behavior.

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
