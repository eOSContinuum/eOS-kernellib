<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Building

eOS-kernellib has no build step of its own. The kernel layer is LPC source that DGD compiles at runtime. The build work in this guide is for the [DGD] driver, which is a build-time dependency.

## DGD

DGD is the LPC runtime that loads and executes the kernel layer. eOS-kernellib targets DGD 1.7.x.

### Standard build

```sh
git clone https://github.com/dworkin/dgd.git
cd dgd/src
make install
```

The driver binary lands at `dgd/bin/dgd`. Run `make clean` from `dgd/src` to start over if a build dirties the tree.

Verify the driver built by running it without arguments; it prints a usage line.

```sh
./bin/dgd
```

### macOS Command Line Tools

If macOS is the build host and only the Xcode Command Line Tools are installed (no full Xcode), `make` fails on `yacc` because `/usr/bin/yacc` is an Xcode-select stub that exits unless Xcode is present. Invoke `bison` at full path explicitly:

```sh
make YACC="/Library/Developer/CommandLineTools/usr/bin/bison -y" install
```

The Command Line Tools' `bison` binary at the path above (GNU Bison 2.3) runs correctly when invoked directly.

### Other platforms

The DGD source compiles on Linux, FreeBSD, and other POSIX-compatible systems with a working C toolchain. Platform detection happens via `uname -s` at the top of `dgd/src/Makefile`.

## eOS-kernellib

There is no build step. DGD compiles the LPC source under `src/` at runtime, on first load and on hot-reload requests.

Verify the kernel layer compiles by running the driver against `example.dgd` per the steps in `doc/getting-started.md`. Compile errors surface in the driver's standard output during boot.

## State and snapshot files

`example.dgd` references swap and snapshot files under `../state/`. Create the directory before starting the driver:

```sh
mkdir -p state
```

The swap file is recreated on each boot. The snapshot file persists across boots and stores the runtime's object graph at the last `dump_interval` checkpoint.

[DGD]: https://github.com/dworkin/dgd
