# Your first Vault-persisted entity

A hands-on tutorial, continuing from [first-http-endpoint.md](first-http-endpoint.md). Your key-value service survives a restart already -- [first-application.md](first-application.md) section 6 proved it without a single save call. Here you give one of its keys a second kind of durability: a typed record written to its own XML file on disk, readable outside the runtime, and able to outlive the image entirely.

**Audience**: a reader who has completed [first-http-endpoint.md](first-http-endpoint.md) (the `KV` domain exists, `"lang"` is stored, the console is claimed). No LPC beyond what is shown. Every command is shown with its expected output.

**What you'll have at the end**: your domain registered as a Vault node, a schema you declared, an entity clonable whose state the platform marshals for you, one key stored as XML you can read with `cat`, that record reloaded after the in-image copy was destroyed, and -- the point of the whole tutorial -- the same record still there after a cold boot with no snapshot at all.

**Editing this tutorial**: this transcript runs against a live boot on every PR (`scripts/tutorial-smoke.sh`). After changing a command or its expected output here, rerun `DGD_BIN=<dgd> scripts/tutorial-smoke.sh` and recapture the changed block from the live session rather than hand-editing the expected output -- `scripts/README.md` (the `tutorial-smoke.sh` section) documents the fence-language whitelist and the anchor-sentence phrasings the parser keys on.

## 1. Why the snapshot is not the whole story

The persistence you already have is the image: DGD writes the entire object graph to `state/snapshot`, and your mapping comes back with it. That is a strong guarantee and it costs you nothing, but it has a shape worth naming. The snapshot is one opaque file covering everything at once. You cannot read one record out of it, hand a single entity to another system, diff yesterday's copy against today's, or recover one object after losing the rest.

The Vault is the other half. It writes each named entity to its own XML file under the Vault's data tree, through a schema you declare. The image still carries your live objects; the XML is a durable, inspectable copy that does not depend on the image existing. [`evaluating.md`](evaluating.md) prices this as the platform's exit cost, and this tutorial is that row made concrete.

## 2. Make the domain a Vault node

A domain joins the Vault by inheriting `~Vault/lib/vault_node` and registering a filesystem root. Create the file on the host.

`src/usr/KV/sys/kv_vault.c`, the domain's Vault node:

```c
# include <type.h>

inherit "~Vault/lib/vault_node";

static void create()
{
    call_out("install", 0);
}

void install()
{
    object node;

    ::create("/usr/KV/data/entries");

    node = clone_object("/usr/Schema/obj/schema_node");
    node->set_name("KV", "Entry");
    node->add_attribute("key", "lpc_str", "query_key");
    node->add_attribute("value", "lpc_str", "query_value");
    node->add_callback("set_key", "key");
    node->add_callback("set_value", "value");
}
```

Two things are happening, and the `call_out` is the one that will bite you if you skip it.

`::create("/usr/KV/data/entries")` registers this object with the Vault daemon as a participating node. `install` then declares a schema: `KV:Entry` with two string attributes, each naming the getter the marshaler calls to read it and the setter it calls to write it back. A schema is not a table definition. It is a marshaling shape -- the list of attributes the platform walks when it turns your object into XML and back.

The `call_out` exists because `/usr/System/initd` compiles the domains in alphabetical order. `KV` sorts before `Schema` and `Vault`, so at cold boot your domain's `create()` runs before either daemon exists, and registering there fails the whole boot. A zero-delay `call_out` fires after every domain's initd has returned, which is the first moment both are up. Section 6 shows exactly what the failure looks like if you are curious enough to try it without.

Compile it, then call `install` yourself:

```text
# compile /usr/KV/sys/kv_vault.c
$0 = </usr/KV/sys/kv_vault>
# code "/usr/KV/sys/kv_vault"->install()
$1 = nil
```

The console `compile` verb defers `create()` until the object's first use, exactly as in [first-application.md](first-application.md) section 3 -- so nothing has scheduled the `call_out` yet, and you call `install` directly. From the next cold boot onward the initd path does this for you.

## 3. The entity

The Vault stores objects, not mappings. Your keys need a clonable to live in: one that carries a logical name, names its schema, and implements the accessors that schema declared.

`src/usr/KV/obj/entry.c`, the entity clonable:

```c
# include <type.h>

inherit "/lib/util/named";

private string _key;
private string _value;

string queryStateRoot()
{
    return "KV:Entry";
}

string query_key()   { return _key; }
string query_value() { return _value; }

void set_key(string val)   { _key = val; }
void set_value(string val) { _value = val; }
```

`/lib/util/named` supplies `set_object_name`, the logical name the Vault uses as both the on-disk path and the inverse-lookup key. `queryStateRoot()` returns the schema name you registered in section 2, which is how a stored object finds its own marshaling shape. The four accessors are the callbacks the schema named: nothing else in the file knows about XML, because the marshaling is the platform's job, not yours.

```text
# compile /usr/KV/obj/entry.c
$2 = </usr/KV/obj/entry>
```

## 4. Teach the service to persist a key

Your daemon can now turn a key into an entity. Add three methods to `src/usr/KV/sys/kv_daemon.c`, above `query_counter`:

```c
object persist(string key)
{
    object entry;

    entry = clone_object("/usr/KV/obj/entry");
    entry->set_key(key);
    entry->set_value(store[key]);
    entry->set_object_name("KV:entry:" + key);
    "/usr/Vault/sys/vault"->store(entry);
    return entry;
}

object reload(string key)
{
    "/usr/Vault/sys/vault"->spawn_one_by_name("KV:entry:" + key);
    return "/usr/Index/sys/index_daemon"->query_object("KV:entry:" + key);
}

int forget(string key)
{
    object entry;

    entry = "/usr/Index/sys/index_daemon"->query_object("KV:entry:" + key);
    if (entry) {
        destruct_object(entry);
        return 1;
    }
    return 0;
}
```

`persist` clones an entry, fills it from the store, names it, and hands it to the Vault. `forget` destroys the in-image copy and leaves the file alone.

`reload` is worth reading twice, because its shape is not the one you would guess. `spawn_one_by_name` returns nothing: it is a command, not a lookup, and it reconstructs the object as a side effect. The object you get back comes from the Index daemon, which is where the Vault registers a restored object's logical name. Writing `return VAULT->spawn_one_by_name(...)` compiles, runs, and hands you `nil` every time.

Recompile the daemon into the running image, the same hot-fix move as [first-application.md](first-application.md) section 5:

```text
# compile /usr/KV/sys/kv_daemon.c
$3 = </usr/KV/sys/kv_daemon>
```

## 5. Store one key, and read it on disk

```text
# code "/usr/KV/sys/kv_daemon"->persist("lang")
$4 = </usr/KV/obj/entry#237>
```

Your clone number will differ from `#237`; clone indices are platform-global. The store wrote a file. Read it back through the platform:

```text
# code read_file("/usr/Vault/data/vault/KV/entry/lang.xml")
$5 = "<clone program=\"/usr/KV/obj/entry\">\n  <KV:Entry key=\"lang\" value=\"LPC\"/>\n</clone>\n"
```

Unescaped, that file is three lines:

```text
<clone program="/usr/KV/obj/entry">
  <KV:Entry key="lang" value="LPC"/>
</clone>
```

That is your key-value pair, on disk, in a format with no runtime in it. The path is the logical name with colons as directory separators, under the Vault's own data tree -- `src/usr/Vault/data/vault/KV/entry/lang.xml` from the repository root, so `cat` it from the host if you want to see it outside the platform. `<clone>` is the root element because an entry is a clone; a one-of-a-kind daemon stores as `<object>` instead, and the element name is how the Vault knows which way to rebuild it.

## 6. The round trip

Destroy the in-image copy and bring it back from the file:

```text
# code "/usr/KV/sys/kv_daemon"->forget("lang")
$6 = 1
# code "/usr/KV/sys/kv_daemon"->reload("lang")->query_value()
$7 = "LPC"
```

The object that answered is not the one you destroyed. It was rebuilt from XML: the Vault read the file, cloned a fresh entry, and dispatched `set_key` and `set_value` from the attributes, which is the same schema walk as section 5 run backwards.

Your mapping is untouched through all of this -- the two kinds of state are independent:

```text
# code "/usr/KV/sys/kv_daemon"->get("lang")
$8 = "LPC"
```

## 7. Outliving the image

Everything so far still had the image. Now take it away.

For the initd to rebuild your domain at a cold boot, it has to compile the two new files. In `src/usr/KV/initd.c`, replace the whole `create()` function:

```c
static void create()
{
    ::create();
    compile_object("sys/kv_daemon");
    compile_object("sys/kv_vault");
    compile_object("obj/entry");
}
```

Stop the platform. `reboot` snapshots and exits, exactly as it did in [first-application.md](first-application.md) section 6:

```text
# reboot
```

Now delete the snapshot it just wrote. This is not the restart-from-snapshot the earlier tutorials did -- there is deliberately no image to come back to:

```sh
rm -f state/snapshot state/snapshot.old
```

Boot with no snapshot argument at all. This is a cold boot: the driver compiles your domain from source, your initd runs, and `install` registers the node and schema from the `call_out`.

```sh
/path/to/dgd/bin/dgd example.dgd
```

Reconnect and look at what survived:

```text
# code "/usr/KV/sys/kv_daemon"->size()
$0 = 0
# code "/usr/KV/sys/kv_daemon"->get("lang")
$1 = nil
```

The store is empty. Every key you put in it is gone, because the mapping lived in the image and you deleted the image. Now ask the Vault:

```text
# code "/usr/KV/sys/kv_daemon"->reload("lang")->query_value()
$2 = "LPC"
# code "/usr/KV/sys/kv_daemon"->reload("lang")->query_key()
$3 = "lang"
```

The record came back from a file. That is the distinction this tutorial exists to make: orthogonal persistence keeps your objects alive across a restart for free, and the Vault keeps chosen entities alive across losing the image, in a format another program can read.

Neither replaces the other. The image is where your service runs; the Vault is where the data you cannot afford to lose also lives.

## What you just used

| Section | Mechanism | Depth |
|---|---|---|
| 2 | Vault node registration, and boot ordering across domains | [vault-applications.md](vault-applications.md) |
| 2 | Schema as a marshaling shape, not a table definition | [schema.md](schema.md) |
| 3 | Logical names and the state root | [vault-applications.md](vault-applications.md) |
| 5 | Store, and the on-disk XML shape | [xml.md](xml.md) |
| 6 | Spawn by name, and Index-mediated lookup | [vault-applications.md](vault-applications.md) |
| 7 | Image persistence versus durable entity storage | [persistence.md](persistence.md) |

## Cleaning up

`src/usr/KV` is removed by `scripts/tutorial-smoke.sh` on every run, and the Vault's data tree under `src/usr/Vault/data` goes with any clean-slate reset -- copy anything you want to keep before running the harness. The finished files from this tutorial live in [`examples/kv-tutorial/`](../examples/kv-tutorial/) alongside the ones the earlier tutorials build.

## Where to next

- **[vault-applications.md](vault-applications.md)** is the reference behind every line here: the participating-domain contract in full, the singleton storage shape, cross-object references, schema evolution, and what the reference application exercises that this tutorial does not.
- **[schema.md](schema.md)** covers the attribute types beyond `lpc_str`, and the property-table shape that needs no per-app schema at all -- the shorter path when your durable state already lives in a property table.
- **[persistence.md](persistence.md)** states what the image carries and what it does not, next to the backup and restore mechanics for both kinds of state.
- **[first-composition.md](first-composition.md)** is the other continuation of [first-http-endpoint.md](first-http-endpoint.md): registered names, a secondary index, an audit observer inside the atomic write, and a capability-gated admin route. It needs the crypto module, which this tutorial does not.
