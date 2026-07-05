# Kernel reference

Man-page-style reference for the kernel layer's modified API surface, organized in the upstream kernel-library convention. The pages are extensionless plain-text man pages, not Markdown -- they read best as fixed-width text.

- [`overview`](overview) -- the kernel library's design rationale: resource control, file security, user management, and the tier discipline the platform rests on.
- `efun/` -- kernel-layer overrides of DGD's built-in functions (the auto object wraps these, so what an application program calls differs from the raw kfun: access checks, resource accounting, path normalization). One page per function:
  [`call_limited`](efun/call_limited),
  [`call_other`](efun/call_other),
  [`call_out_other`](efun/call_out_other),
  [`call_trace`](efun/call_trace),
  [`clone_object`](efun/clone_object),
  [`compile_object`](efun/compile_object),
  [`destruct_object`](efun/destruct_object),
  [`file_info`](efun/file_info),
  [`find_object`](efun/find_object),
  [`get_dir`](efun/get_dir),
  [`new_object`](efun/new_object),
  [`retrieve_atomic_messages`](efun/retrieve_atomic_messages),
  [`send_atomic_message`](efun/send_atomic_message),
  [`status`](efun/status),
  [`tls_get`](efun/tls_get),
  [`tls_set`](efun/tls_set)
- `lfun/` -- the per-object function contract: [`create`](lfun/create) (the kernel calls it; objects define it) and [`query_owner`](lfun/query_owner) (the auto object predefines it; objects call it).
- `hook/` -- the driver and user-daemon hook contracts: [`driver`](hook/driver), [`userd`](hook/userd).

SEE ALSO references of the form `kfun/<name>` point at DGD's own kfun documentation, maintained in [`dworkin/lpc-doc`](https://github.com/dworkin/lpc-doc); those pages are not part of this repository.

These pages document the kernel contract at the function level. For task-oriented introductions, start with [getting-started.md](../getting-started.md) and [lpc-essentials.md](../lpc-essentials.md); for the platform's own library catalog, see [kernel-libraries.md](../kernel-libraries.md).
