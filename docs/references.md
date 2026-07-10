# References and further reading

Citations referenced inline across the eOS-kernellib doc set, plus the upstream documentation a reader following the citations may want to consult. Each entry carries a stable anchor. Inline citations in other docs link to the anchor here.

**Audience**: a reader following a citation from one of the other docs, or surveying the literature this kernel layer rests on.

## Academic literature

### Orthogonal persistence

<a id="atkinson-morrison-1995"></a>**Atkinson, M. P. and Morrison, R.** *Orthogonally Persistent Object Systems.* VLDB Journal 4(3), 1995. The canonical academic statement of orthogonal persistence as an architectural property: the same code operates on transient and persistent values. The persistence machinery is the runtime's concern, not the application's. Specific formulations cited across the doc set: the three principles (persistence independence, data type orthogonality, persistence identification, §2.2.2); the cost estimate that roughly thirty percent of a typical database application's code is mapping and transfer plumbing (p. 333, citing King 1978); the transient-versus-long-lived lifetime framing (Table 2, p. 325); the treatment of untyped memory-image stores as incomplete orthogonal persistence (p. 354); and the taxonomy of implementation architectures whose deepest tier (the persistent world) is where DGD sits (§4.1). Cited in [persistence.md](persistence.md) Why orthogonal persistence, Orthogonal persistence as architectural property, and Compared with common alternatives, and in [runtime-primitives.md](runtime-primitives.md) §3.

### Capability systems

<a id="keykos-eros"></a>**KeyKOS** (Hardy, 1985; Bomberger et al., 1992) and **EROS** (Shapiro, Smith, Farber, 1999). The capability-systems literature that extended the orthogonal-persistence model with runtime-enforced capability-based access control. The two systems together establish the lineage the kernel layer's tier model draws on: capability bounds enforced at the runtime layer, not the application layer. Cited in [persistence.md](persistence.md) Orthogonal persistence and [capability.md](capability.md) (the tier-mediation-versus-object-capability distinction).

Representative reading: Jonathan Shapiro and Jonathan Adams, *EROS: A Fast Capability System*, SOSP 1999. The KeyKOS and EROS project papers are collected in their respective archive sites and in the systems-research literature index.

## DGD mailing-list citations

The DGD mailing list and its predecessor MUD-Dev mailing list carry contemporary documentation of DGD's properties. The archives are at <https://mail.dworkin.nl/pipermail/dgd/> and <https://mail.dworkin.nl/pipermail/mud-dev-archive/>.

<a id="allen-dgd-2000"></a>**Christopher Allen, "DGD Description", MUD-Dev mailing list, 2000-04-11.**
<https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html>

The canonical contemporary statement of DGD's substrate properties. Names atomicity ("atomic function calls allow full system-state rollback in the event of a run-time error"), persistence ("DGD maintains persistence as a characteristic of its runtime environment"), and statedump-based snapshot ("full system state dump files implement persistence across reboots as well as snapshot-style state backups"). Cited in the root README Heritage section, [runtime-primitives.md](runtime-primitives.md) §1 atomicity and §3 persistence, [persistence.md](persistence.md) Orthogonal persistence, [lpc-essentials.md](lpc-essentials.md) Atomicity, and [code-lifecycle.md](code-lifecycle.md) Hot reload.

<a id="croes-static-2003"></a>**Felix Croes, "DGD and the static keyword", DGD mailing list, 2003-04.**
<https://mail.dworkin.nl/pipermail/dgd/2003-April/003390.html>

Authoritative statement on the `static` storage-class modifier's runtime semantics: `static` affects `save_object` (the variable is not written to the per-object save file) and is otherwise a normal variable. The statedump captures the in-memory image regardless of `static`. Cited in [lpc-essentials.md](lpc-essentials.md) Type modifiers.

<a id="croes-hydra-2010"></a>**Felix Croes, Hydra mailing-list note on extension-loaded codepaths, 2010-08.**
<https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html>

Authoritative statement on the statedump-binding constraint for host-driver extensions: a snapshot taken with a kfun extension active requires the same extension to restore. The constraint is a durable architectural commitment, not an opt-in convenience. Cited in [persistence.md](persistence.md) Persistence under host-driver extensions and [operations.md](operations.md) Loading host-driver extensions. [architecture.md](architecture.md) Host-driver extensions restates the constraint.

## Upstream documentation

### DGD itself

<a id="dgd-repo"></a>**[dworkin/dgd](https://github.com/dworkin/dgd)**: the DGD driver this kernel layer runs on. Felix Croes' active development repository.

<a id="dgd-doc-extensions"></a>**DGD `doc/extensions.md`** in the dgd repository. Documents the `.dgd` config's `modules = ([ path : config ])` mapping for loading kfun modules, and points at [dworkin/lpc-ext](https://github.com/dworkin/lpc-ext) for the extension interface itself.

### LPC language reference

<a id="lpc-doc"></a>**[dworkin/lpc-doc](https://github.com/dworkin/lpc-doc)**: Felix Croes' canonical LPC language reference. Pinned in DGD's `.gitmodules` at commit `403cd0bc52ffd8ef57812d7cf5510e20e0566d81` (verified at the 2026-05-14 cited reading). The repository contains:

- **`LPC.md`**: the formal language specification. Three main sections: Introduction, Environment, Language. §3.4.2 enumerates the eight LPC types (nil, int, float, string, object, mapping, mixed, void). §3.4.3 enumerates the four type modifiers (private, static, nomask, atomic).
- **`kfun/`**: the kfun catalog.
- **`Introduction.md`**: server-architecture orientation (not a pedagogical "learn LPC" doc).

[lpc-essentials.md](lpc-essentials.md) cites `LPC.md` section numbers throughout for the formal language spec.

### Kernellib lineage

<a id="kernellib-ancestor"></a>**[ChatTheatre/kernellib](https://github.com/ChatTheatre/kernellib)**: the kernellib fork this kernel layer descends from. Released under CC0 1.0 with public-domain declarations from Skotos Tech, Dyvers Hands, Christopher Allen, and Noah Gibbs. Felix Croes' upstream kernellib was declared public domain in 2016 ("The kernellib is and has always been in the public domain"). The kernellib fork established the tier discipline (kernel / system / user), the auto-inheritance pattern, and the per-owner resource model the platform's capability machinery rests on.

<a id="kernellib-doc"></a>**[ChatTheatre/kernellib-doc](https://github.com/ChatTheatre/kernellib-doc)**: collaborator-facing documentation for the kernellib fork. Reference reading for the kernel-layer object-lifecycle event surface. Patterns documented there informed [code-lifecycle.md](code-lifecycle.md) (the object-manager event table) and [admin-console.md](admin-console.md). Note that the event surface documented there is the classic twelve-event convention. This kernel dispatches a smaller surface, documented in code-lifecycle.md.

### Comparative reference

<a id="skotos"></a>**[ChatTheatre/SkotOS](https://github.com/ChatTheatre/SkotOS)**: the largest LPC application built on this lineage (1999-2018, rights held by Christopher Allen since Skotos Tech's 2019 close). Pattern-level reference for application authoring on top of a kernellib-derived kernel, not a foundation, since SkotOS is downstream of the kernellib. The kernel layer's HTTP/1 application pattern took the daemon-registry shape from SkotOS' equivalent surface.

## Where to next

- [glossary.md](glossary.md): definitions for terms used inline across the doc set.
- [getting-started.md](getting-started.md): first-time install of DGD plus this repository.
- [architecture.md](architecture.md): the structural reference for the platform.
