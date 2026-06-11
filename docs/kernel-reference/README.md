# Kernel reference

Man-page-style reference for the kernel layer's modified API surface, organized in the upstream kernel-library convention:

- `efun/` -- kernel-layer overrides of DGD's built-in functions (the auto object wraps these, so what an application program calls differs from the raw kfun: access checks, resource accounting, path normalization). One page per function.
- `lfun/` -- functions an object may define that the kernel calls (`create`, `query_owner`).
- `hook/` -- the driver and user-daemon hook contracts.
- `overview` -- the kernel library's design rationale: resource control, file security, user management, and the tier discipline the platform rests on.

These pages document the kernel contract at the function level. For task-oriented introductions, start with [getting-started.md](../getting-started.md) and [lpc-essentials.md](../lpc-essentials.md); for the platform's own library catalog, see [kernel-libraries.md](../kernel-libraries.md).
