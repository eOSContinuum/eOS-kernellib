# Kernel reference

The function-level kernel contract in one page: what the kernel layer changes about DGD's built-ins (the efun overrides), the per-object functions and hooks the platform calls on your objects, and the routers that send every other callable kind to its signature home.

**Audience**: an LPC author needing the function-level kernel contract -- what the kernel layer changes about DGD's built-ins, and the per-object functions and hooks it calls.

## Where signatures live

The sections on this page are one of several signature homes. From "I need the signature of X", route by what kind of callable X is:

| Callable kind | Examples | Home |
|---|---|---|
| Kernel override of a DGD built-in | `clone_object`, `call_out`, `status` | [Efun overrides](#efun-overrides) below |
| Unmodified DGD built-in (kfun) | `sizeof`, `explode` | [`dworkin/lpc-doc`](https://github.com/dworkin/lpc-doc) |
| Per-object contract; driver and userd hooks | `create`, `patch`, `query_owner`; `runtime_error` | [Per-object functions](#per-object-functions-lfuns) and [Hooks](#driver-and-user-daemon-hooks) below |
| Inheritable library class or utility | `KVstore`'s `set`, `Continuation`'s `chain`, `base64` `encode` | [`kernel-libraries.md`](kernel-libraries.md), per-class blocks |
| Property surface on a host object | `set_property`, `query_prefixed_properties` | [`kernel-libraries.md`](kernel-libraries.md), the `/lib/util/properties.c` block |
| Merry daemon LFUN | `register_observer`, `dispatch_set`, `batched_set` | [`dispatcher.md`](dispatcher.md), Application surface |
| Merryfun (called from Merry source) | `Set`, `BatchedSet`, `Call` | [`merry-language.md`](merry-language.md), Merryfun call surface |
| Console verb | `code`, `upgrade`, `log` | [`admin-console.md`](admin-console.md), the alphabetical verb appendix |
| System daemon API | `objectd`, `upgraded`, `errord`, `logd` | [`system-daemons.md`](system-daemons.md) |
| HTTP API class | `HttpRequest`, `Http1Server`, `Http1Client` | [`http-applications.md`](http-applications.md), API signatures |

`See also` references naming a DGD kfun point at DGD's own kfun documentation, maintained in [`dworkin/lpc-doc`](https://github.com/dworkin/lpc-doc); those pages are not part of this repository.

## Where the platform calls you

The inverse router: the hooks and contracts the platform invokes on objects you write, and where each contract is stated.

| The platform calls | When | Contract home |
|---|---|---|
| `create()` | On instantiation of every object | [create](#create) below |
| `patch()` | Through the `call_touch` gate after an upgrade marks the clone | [patch](#patch) below; the migration doctrine is [`code-lifecycle.md`](code-lifecycle.md) Touch |
| The driver hooks (`runtime_error`, `compile`, and the rest) | Driver events, on the registered manager objects | [driver](#driver) below |
| The userd hooks | Connection events, on registered telnet/binary managers | [userd](#userd) below |
| Object-manager events (`compile`, `clone`, `destruct` notifications) | On the registered object manager, per lifecycle event | [driver](#driver) below (`set_object_manager`); the event surface narrative is [`code-lifecycle.md`](code-lifecycle.md) |
| An HTTP server object's request methods | Per parsed request on your `Http1Server` subclass | [`http-applications.md`](http-applications.md), the five platform contracts |
| A Merry script bound on a property | At the bound timing when the property changes | [`dispatcher.md`](dispatcher.md) timings; [`merry-language.md`](merry-language.md) for what the script may call |

## Kernel overview

### Motivation

The kernel library was written to solve the technical problems encountered when writing a mudlib for users who will have programming access.  It deals with resource control, file security and user management.  The library is designed to be fully configurable, and should not have to be modified for use on any system.  It can be used for both persistent and non-persistent systems.

Throughout this document, a game mudlib point of view will be taken, but the kernel library can be used for any type of multi-user system.

### Directory structure

The kernel library itself resides in /kernel.  Subdirectories are:

```
    /kernel/data
		    This is where kernel information is saved in non-persistent
		    systems, notably access levels and passwords for
		    programmers.
    /kernel/lib
		    Inheritable objects, such as the auto object.  None of
		    these objects can be inherited directly by programmers.
    /kernel/lib/api
		    APIs for kernel manager objects.
    /kernel/obj
		    All cloned kernel objects, such as the default user object,
		    reside in this directory.
    /kernel/sys
		    Kernel manager objects, such as the driver object, access
		    manager, resource manager, etc.
```

The kernel imposes the following directory structure on the rest of the system:

```
    /doc
		    Documentation.
    /doc/kernel
		    Documentation about the kernel library.
    /include
		    System include files.
    /include/kernel
		    Kernel include files.
    /usr
		    The programmers' directories are subdirectories of /usr.
    /usr/System
		    The system directory, which defines the basic behaviour
		    of the mudlib above the kernel level.  Files in this
		    directory have special permissions and may inherit from
		    /kernel/lib.
```

#### Restrictions on use of objects

The kernel imposes strict limits on the use of objects. An inherited object can never itself be accessed as an object.  It is not possible to call a function in it, and special functionality exists to compile and destruct it.

Inheritable objects must have "lib" as a path component.  Objects without "lib" in their path are not inheritable, and can be used for such things as rooms or other mudlib objects.

### Resource control

The kernel library includes a system to keep track and impose limits on resource usage.  A "resource" can be anything of which there is a limited supply, such as ticks, amount of objects, or space used by files.  The resource control system is generic, and new resources can be defined or removed on the fly.

Resource information is maintained per programmer.  Global limits can be imposed, and exceptions can be made.  Programmers themselves can create new resources.

### File security

There are 3 access levels: read, write, and access-granting.  By default, everyone has read access in all directories outside /usr.  Every object in /kernel and /usr/System has global access.  Programmers have access-granting access to their own directory.  A programmer's objects have the same global read access as the programmer, the same write access in the programmer's directory, and can clone and inherit from directories where they have read access.  Objects neither in /usr nor in /kernel only have read access in the usual directories, and cannot compile or clone new objects at all.

A programmer's access can be changed: administrators have access-granting access to the root directory.  However, the programmer's access has no effect on the access of the programmer's objects.

### Application Programmer Interfaces

Each kernel manager object has a corresponding inheritable API.  The API provides functions which, when called, are routed to the manager object. The manager object checks each call to make sure it is called by the API, only.

The manager APIs are inheritable only by objects in /usr/System.  Calls are unrestricted, so any security must be provided by the inheriting object.

### User management

The kernel library provides a bare-bones user manager and user object. The user object provides basic communication commands for unprivileged users, development commands for users with programming access, and user and resource management commands for administrators.  Both the user manager and the user object are minimal, and intended to be extended or replaced altogether.

Initially, users can login on either a telnet or binary port.  The only existing user is "admin", who has administrator access.  Using the "admin" account, other users can be granted programming access.

### Standard admin_console

The kernel library includes a "admin_console", an object which defines the basic developer commands for programmers.  The admin_console is inheritable, and can thus be used as the basis for a more advanced development tool.

### Extending the kernel library

The kernel library was written in such a way that it can easily be extended without modifying any kernel objects.  The user manager and driver object contain hooks that allow routing of some calls to different objects.

When you begin building, start with making a new user object.  This object should reside in /usr/System/obj.  The user object must inherit /kernel/lib/user.  Next, create a telnet connection manager which can be installed using "/kernel/sys/userd"->set_telnet_manager(port, obj). Finally, create /usr/System/initd.c which handles system-specific initialization (for instance, the telnet connection manager must be installed from its create() function).

Persistence is on by default: SYS_PERSISTENT is defined in /include/config.h.  If you want a non-persistent mud instead, remove that define.

Next step: reboot.  If everything works, you can start extending your user object, over which you now have full control.

## Efun overrides

### call_limited

```c
mixed call_limited(string function, mixed args...)
```

Call a function in the current object, using the resource limits of the current object's owner.

See also: [call_other](#call_other), [call_out_other](#call_out_other)

### call_other

```c
mixed call_other(mixed obj, string function, mixed args...)
```

Call a function in an object.  The first argument must be either an object or a string.  If it is a string, call_object() will be called in the driver object to get the corresponding object. Only non-private functions can be called with call_other().  If the function is static, the object in which the function is called must be the same as the object from which the function is called, or the call will fail. Any additional arguments to call_other() will be passed on to the called function. In LPC, obj->func(arg1, arg2, argn) can be used as a shorthand for call_other(obj, "func", arg1, arg2, argn).

**Errors.** An error will result if the first argument is not an object and not a string, or if the first argument is a string, but the specified object is either uncompiled or an object with "lib" as a path component. Calling a function that does not exist, or a function that cannot be called with call_other() because it is private or static, does not result in an error but returns the value nil.

See also: [call_limited](#call_limited), [call_out_other](#call_out_other), [clone_object](#clone_object), [destruct_object](#destruct_object), function_object (DGD kfun, dworkin/lpc-doc)

### call_out

```c
int call_out(string function, mixed delay, mixed args...)
```

Start a callout: schedule the given function in the current object to be called delay seconds from now, and return a handle that identifies the pending callout.  delay is an int or a float; a float schedules a sub-second callout.  Any further arguments are stored and passed to the function when the callout fires.

The kernel layer routes the callout through its resource-limited execution gate, so that when the function runs it is accounted against the resource limits of the object's owner, the same gate call_limited() uses.  Callouts started from a kernel object (one whose name begins with /kernel/) are scheduled directly, without the gate.

**Errors.** Starting a callout for a function defined in the kernel's auto object (other than create()), or in a light-weight object (one with a name of the form "object_name#-1"), results in an error.

See also: [call_out_other](#call_out_other), [call_limited](#call_limited), call_out (DGD kfun, dworkin/lpc-doc)

### call_out_other

```c
int call_out_other(object obj, string function, mixed delay, mixed args...)
```

Start a callout in an object.  Only non-private functions can be called with call_out_other().  If the function is static, the object in which the function is called must be the current object. Apart from the object argument, the arguments are the same as for call_out().

**Errors.** Calling a function that does not exist, or a function that cannot be called with call_out_other() because it is private or static, results in an error.  Starting a callout for a function defined in the kernel's auto object (other than create()), or in a light-weight object (one with a name of the form "object_name#-1"), also results in an error.  The callout itself executes through the kernel's resource-limited execution gate, the same as call_out().

See also: [call_limited](#call_limited), [call_other](#call_other), call_out (DGD kfun, dworkin/lpc-doc)

### call_trace

```c
mixed *call_trace(varargs mixed index)
```

Return the function call trace as an array.  The elements are of the following format:

```
({ objname, progname, function, line, extern, arg1, ..., argn })
```

The line number is 0 if the function is in a compiled object. Extern is 1 if the function was called with call_other(), and 0 otherwise. The offsets in the array are named in the include file <trace.h>. The last element of the returned array is the trace of the current function. If the optional index argument is given, only the trace frame at that offset is returned, as a single array in the format above, rather than the full array of frames.

**Access.** Unless the creator of the current object is "System", if the creator of the current object is not the same as the creator of the program containing a function, the arguments are omitted.

See also: previous_object (DGD kfun, dworkin/lpc-doc), previous_program (DGD kfun, dworkin/lpc-doc)

### clone_object

```c
object clone_object(string master, varargs string owner)
```

Create a clone of the specified object with an unique name of the form "object_name#1234".  The cloned object must not itself be a clone.  The new object is returned.  The create() function will be called in the cloned object immediately. If the optional second argument is specified and non-zero, the calling program must be in /kernel/ or /usr/System/; any other caller passing an owner gets an error.  If the argument is omitted or zero, the new object will have the same owner as the current object.

**Access.** If the file to be cloned is located under /kernel/, only a /kernel/ object may clone it.  There is no read-access check for the current object beyond this restriction.

**Errors.** If the number of existing objects is equal to the value of the ST_OTABSIZE field of the array returned by status(), where ST_OTABSIZE is defined in the include file <status.h>, attempting to clone a new object will crash the system.

See also: [call_other](#call_other), [destruct_object](#destruct_object), [new_object](#new_object)

### compile_object

```c
object compile_object(string file, string source...)
```

Compile an object from a LPC file, specified by the first argument with ".c" appended.  If the optional source argument is supplied, the object is compiled from that string, instead.  The returned object will have the file string as name. If the object to be compiled already exists and is not inherited by any other object, it and all of its clones will be upgraded to the new version.  Variables will be preserved only if they also exist in the new version and have the same type; new variables will be initialized to their type defaults (0 for int, 0.0 for float, nil otherwise).  The actual upgrading is done immediately upon completion of the current task. If the new object has "lib" as a path component, it can only be inherited and nil is returned.  Otherwise, it can be cloned.

**Access.** Unless the current object's creator is "System", the current object needs write access to the file to be compiled; read access is enough when the file has "lib" as a path component or has no creator, no inline source is supplied, and the file is not under /kernel/.  Supplying inline source for a file under /kernel/ is denied even to "System"-creator objects.

See also: object_name (DGD kfun, dworkin/lpc-doc)

### destruct_object

```c
int destruct_object(mixed obj)
```

Destruct the object given as the argument, which can be an object or the name of an object.  Any value holding the object will immediately change into nil, and the object will cease to exist. If an object destructs itself, it will cease to exist as soon as execution leaves it.  If the last reference to a master object is removed (including cloned objects and inheriting objects), the function remove_program(path, timestamp, index) will be called in the driver object. Return 1 if the object existed and was destructed, 0 otherwise.

**Access.** Unless the creator of the current object is "System", an object can only be destructed if it has the same owner as the current object.

**Errors.** Objects destructing themselves may not do certain things between the time of destruction and the time the object will cease to exist.  Most notably, call_other() may not be used from destructed objects.

See also: [call_other](#call_other), [clone_object](#clone_object)

### file_info

```c
mixed *file_info(string file)
```

Get information about a file.  The return value is of the form

```
({ file size, file modification time, object })
```

If a file is a directory, the file size will be given as -2. The object value is set to 1 if the object exists and has "lib" as a path component. If the file doesn't exist, nil is returned.

**Access.** Unless the creator of the current object is "System", the current object must have read access to the file.

See also: [get_dir](#get_dir)

### find_object

```c
object find_object(string obj)
```

The string argument is resolved as a file path, and the object with the resulting name is searched for.  Either the object, if found, or nil is returned. Objects with "lib" as a path component cannot be found by this function.

See also: object_name (DGD kfun, dworkin/lpc-doc)

### get_dir

```c
mixed **get_dir(string file)
```

Get information about a file or files in a directory.  The return value is of the form

```
({ ({ file names }), ({ file sizes }), ({ file mod times }), ({ objects }) })
```

If a file is a directory, the file size will be given as -2. If the last path component of the specified file can be interpreted as a regular expression, all files which match this regular expression are collected.  Otherwise, only the file itself is taken.  If no files match, or if the file is not present, the return value of get_dir() will be ({ ({ }), ({ }), ({ }), ({ }) }). Objects that have "lib" as a path component are replaced with 1 in the object array. The following characters have a special meaning in a regular expression:

```
?	    any single character
*	    any (possibly empty) string
[a-z]   any character in the range a-z
[^a-z]  any character not in range a-z
\c	    the character c, not interpreted as having a special
	    meaning
```

The files will be sorted by file name. Only as many files as specified by status()[ST_ARRAYSIZE], with ST_ARRAYSIZE defined in the include file <status.h>, will be collected.

**Access.** Unless the creator of the current object is "System", the current object must have read access to the file.

See also: [file_info](#file_info)

### make_dir

```c
int make_dir(string path)
```

Create the directory named by path.  The path is first reduced to its minimal absolute form, with a relative path resolved against the current object's directory.  The return value is non-zero on success and zero if the directory could not be created.

The kernel layer wraps the raw operation in file security and resource accounting.  A successful create charges one file block to the directory creator's quota, and runs with tick and stack accounting suspended.

**Access.** Unless the creator of the current object is "System", the current object must have write access to path.  No object may create a directory under /kernel or /include/kernel.

**Errors.** Unless the current object's creator is "System", make_dir() fails with "File quota exceeded" when the directory creator's file-block quota is already at its limit.  A path that fails the access rules above fails with "Access denied".  Calling make_dir() with no current object fails with "Permission denied".

See also: [remove_dir](#remove_dir), [get_dir](#get_dir), make_dir (DGD kfun, dworkin/lpc-doc)

### new_object

```c
object new_object(string master, varargs string owner)
object new_object(object master)
```

Create a new light-weight instance of the specified object with a name of the form "object_name#-1".  If the master object is itself a light-weight object, it will be copied.  Light-weight objects cannot be destructed and are automatically deallocated once the last reference to them is removed.  The new object is returned.  The create() function will be called in the new object immediately. If the optional second argument is specified and non-zero, the calling program must be in /usr/System/ and the first argument must be a master-file path (not an existing light-weight object to copy); any other caller passing an owner gets an error.  If the argument is omitted or zero, the new object will have the same owner as the current object.

**Access.** There is no read-access check: any object may create a new instance of any compiled, non-"lib" master outside /kernel/.

See also: [clone_object](#clone_object)

### remove_dir

```c
int remove_dir(string path)
```

Remove the directory named by path.  The path is first reduced to its minimal absolute form, with a relative path resolved against the current object's directory.  The return value is non-zero on success and zero if the directory could not be removed.

The kernel layer wraps the raw operation in file security and resource accounting.  A successful removal returns one file block to the directory creator's quota, and runs with tick and stack accounting suspended.

**Access.** Unless the creator of the current object is "System", the current object must have write access to path.  No object may remove a directory under /kernel or /include/kernel.

**Errors.** A path that fails the access rules above fails with "Access denied".  Calling remove_dir() with no current object fails with "Permission denied".

See also: [make_dir](#make_dir), [get_dir](#get_dir), remove_dir (DGD kfun, dworkin/lpc-doc)

### retrieve_atomic_messages

```c
string *retrieve_atomic_messages()
```

Retrieve any messages sent from within atomically executed code that was rolled back due to an error.

See also: [send_atomic_message](#send_atomic_message)

### send_atomic_message

```c
void send_atomic_message(string message)
```

Add a string to the list of messages that can be retrieved after an atomic rollback occurs.

**Errors.** If the message starts with the character "*" and also contains a \0 character, an error will result.  A \0 character elsewhere in the message is not rejected.

See also: [retrieve_atomic_messages](#retrieve_atomic_messages)

### status

```c
mixed status(varargs mixed obj, mixed index)
```

Called without an argument, this kfun returns information about resources used by the system, as an array whose fields are described in the include file <status.h>.  The first argument may also be an object, or a string naming one (resolved to that object), in which case resource usage by that object is returned; or an integer, which selects a single field of the system status array directly.  If the optional second argument is given, it selects a single field of the first argument's status array, returning that field's value alone rather than the full array.

**Access.** If the current object is not the owner of the argument object, if any, callout arguments are omitted in the returned status array.

### tls_get

```c
mixed tls_get(mixed index)
```

Get the Task Local Storage value described by the index.  Negative integer indices are reserved for use by the kernel.

See also: [tls_set](#tls_set)

### tls_set

```c
void tls_set(mixed index, mixed value)
```

Set the Task Local Storage value described by the index.  Negative integer indices are reserved for use by the kernel.

See also: [tls_get](#tls_get)

## Per-object functions (lfuns)

### create

```c
void create()
```

This function is called when an object is initialized.

### patch

```c
void patch()
```

The per-clone migration hook. After an upgrade recompiles a clonable master, the upgrade daemon marks each existing clone with `call_touch`; the platform's `nomask` touch gate (`_F_touch()` in the System auto) then calls `this_object()->patch()` -- with a patchtool supplied (`upgrade -p`), eagerly in a post-upgrade sweep, and otherwise on the clone's first reference after the mark. Define it public (the gate reaches it through `call_other`); objects that need no migration simply omit it.

The contract (`code-lifecycle.md` Touch owns the doctrine):

- `patch()` runs at most once per `call_touch`, **before** the next call against the object, inside the calling context's atomic envelope -- a failing patch rolls back.
- It runs on the **newly compiled program**, against the already-remapped dataspace: variables the new source dropped are gone before `patch()` can read them, and variables it added arrive as fresh type defaults (0, 0.0, or nil by type) (`changing-a-running-system.md` Rolling back a release states the consequence for reverse migrations).
- Each `-p` sweep is a fresh `call_touch`, and the sweep re-runs `patch()` even when the recompiled source is unchanged -- so `patch()` must be idempotent: check a format-version property, transform, stamp (`common-tasks.md` Migrate live state after a data-shape change is the recipe).

**See also**: [create](#create); `code-lifecycle.md` Touch; `common-tasks.md` Migrate live state after a data-shape change.

### query_owner

```c
nomask string query_owner()
```

This function is predefined in all objects.  It returns the owner of the object.

## Driver and user-daemon hooks

### driver

```c
string creator(string file)
```

Get the creator of a file.

```c
string normalize_path(string file, string directory, string owner)
```

Normalize a path.

```c
int file_size(string file, varargs int dirflag)
```

**[System only]**

Check size of a file.  If dirflag is TRUE, recursively check size of directory.

```c
void set_object_manager(object objectd)
```

**[System only]**

Install an object manager, in which the following functions will be called afterwards:

- ```c
  void compile(string owner, string path, mapping source, string inherited...)
  ```

  The given object has just been compiled.  Source is a mapping from paths to source code, or to the same paths if no source code was provided.

- ```c
  void compile_failed(string owner, string path)
  ```

  An attempt to compile the given object has failed.

- ```c
  void clone(string owner, object obj)
  ```

  The given object has just been cloned.  Called just before the object is initialized with create().

- ```c
  void destruct(string owner, string path)
  ```

  The given object is about to be destructed.

- ```c
  void remove_program(string owner, string path, int timestamp, int index)
  ```

  The last reference to the given program has been removed.

- ```c
  mixed inherit_program(string from, string path, int priv)
  ```

  The flag `priv' indicates that inheritance is private.  Return either a string for an alternate path of the inherited file, or an array of strings, representing the source code of the inherited file itself. Any other return value will prevent inheritance of the file `path'.

- ```c
  mixed include_file(string compiled, string from, string path)
  ```

  The file `path' (which might not exist) is about to be included by `from' during the compilation of `compiled'.  The returned value can be either a string for the translated path of the include file, or an array of strings, representing the included file itself.  Any other return value will prevent inclusion of the file `path'.

- ```c
  int touch(object obj, string function)
  ```

  An object which has been marked by call_touch() is about to have the given function called in it.  A non-zero return value indicates that the object's "untouched" status should be preserved through the following call.

- ```c
  string call_object(string path)
  ```

  Return a path, or nil if `path' is not a valid first argument for call_other().

```c
void set_error_manager(object errord)
```

**[System only]**

Install an error manager, in which the following functions can be called afterwards:

- ```c
  string runtime_error(string error, int caught, mixed **trace)
  ```

  A runtime error has occurred.  The returned string replaces the error message.

- ```c
  void atomic_error(string error, int atom, mixed **trace)
  ```

  A runtime error has occurred in atomic code.

- ```c
  void compile_error(string file, int line, string error)
  ```

  A compile-time error has occurred.

```c
void message(string str)
```

**[System only]**

Show the given string with send_message().

### userd

```c
void set_telnet_manager(int port, object telnetd)
void set_binary_manager(int port, object binaryd)
void set_datagram_manager(int port, object datagramd)
```

**[System only]**

Install a manager on a specific port for new connections, in which the following functions will be called:

- ```c
  object select(string str)
  ```

  Return a user object, selected by the argument string, which is the first line of input on that connection.

- ```c
  int query_mode(object connection)
  ```

  Return the initial mode for the connection.  MODE_DISCONNECT can be used to terminate the connection immediately.

- ```c
  int query_timeout(object connection)
  ```

  Return a timeout, after which the given connection is closed if no user object has been associated with it yet.  If the timeout is -1, this check is performed immediately.

- ```c
  string query_banner(object connection)
  ```

  Return a login banner for the given connection.  Nil can be returned to indicate that no banner should be shown at all.

---

`See also` lines above that name a DGD kfun refer to DGD's own documentation in [`dworkin/lpc-doc`](https://github.com/dworkin/lpc-doc). For task-oriented introductions, start with [getting-started.md](getting-started.md) and [lpc-essentials.md](lpc-essentials.md); for the platform's own library catalog, [kernel-libraries.md](kernel-libraries.md).
