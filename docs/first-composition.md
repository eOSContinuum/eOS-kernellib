# Growing the service into a composition

A hands-on tutorial, continuing from [first-http-endpoint.md](first-http-endpoint.md). Your key-value service answers the console and the wire; here you grow it into a small composition: a second object kind with real identity, a secondary index maintained in the same atomic write as the data, an audit trail appended by an observer inside that same commit, and an admin operation gated by a platform capability an operator grants. Every mechanism the composite example composes at scale, you will build here at the smallest size that shows it working.

**Audience**: a reader who has completed [first-http-endpoint.md](first-http-endpoint.md) (the `KV` and `WWW` domains exist, `"lang"` stored, routes driven with `curl`). Every command is shown with its expected output. Two references stand behind this tutorial: [application-authoring.md](application-authoring.md) Modeling domain data for the shapes, and [composite-applications.md](composite-applications.md) for the full-scale assembly this points toward.

**What you'll have at the end**: a clonable you wrote with its own per-instance state and a registered name, a tag index that can never disagree with its store, an audit log that un-happens when a write rolls back, a wipe route that refuses without operator-granted authority -- and the persistence proof over all of it.

## 1. Load the crypto module

The finale of this tutorial gates an admin route on a platform capability, and proving *who* is asking takes the identity stack -- whose minting stood down in every boot so far: the `crypto module absent` NOTICE lines from [first-hour.md](first-hour.md) section 1. Load the module now. Build it once (the `crypto` module from the `lpc-ext` repository; `docs/operations.md` Loading host-driver extensions is the recipe), then add one line to your `example.dgd`:

```text
modules		= ([ "/absolute/path/to/lpc-ext/crypto.1.6" : "" ]);
```

One fact decides what happens next, and it is worth learning on purpose: **an extension is a cold-boot fact.** The kfun table is part of the image, fixed when the driver initializes -- a snapshot taken without the module restores fine under a module-loaded driver, but the module's kfuns are not reachable in the restored image (`session mint` answers `session: crypto module not loaded`). The reverse is also a restore precondition: a snapshot taken *with* the module needs it present to restore at all (`docs/operations.md` Backing up and restoring state). So: stop the platform and boot **cold** -- no snapshot argument:

```sh
/path/to/dgd/bin/dgd example.dgd
```

The boot log now says `crypto module present` where the stand-downs were. The cold boot rebuilt from source -- your initds compiled both domains -- but the *runtime* state of the old image (the store's keys) stayed behind in the snapshot. Reconnect and re-seed the one key the rest of this tutorial expects:

```text
# code "/usr/KV/sys/kv_daemon"->put("lang", "LPC")
$0 = nil
# code "/usr/KV/sys/kv_daemon"->get("lang")
$1 = "LPC"
```

(As in the earlier tutorials, `$N` history restarts at `$0` with each console login, and any extra command shifts the later slots -- compare values, not slot numbers.)

## 2. The second object kind

Your store is one mapping in one daemon: uniform rows, no identity. [application-authoring.md](application-authoring.md) Modeling domain data names the fork this tutorial now takes: entities with real identity -- per-instance state, cross-references, independent lifecycle -- are **clones**. You will add named stores: each a clone of one master, holding its own entries and write count, registered under a logical name any code can resolve.

`src/usr/KV/obj/store.c` -- a new file (the `obj/` subdirectory is what makes it clonable):

```c
/*
 * A named store: one clone per store, holding its own entries and its
 * own write count. Clones register a logical name at creation so any
 * domain code can find them again without holding a reference.
 */

inherit "/lib/util/named";

private mapping entries;    /* key : value */
private int writes;         /* this store's write count */

static void create()
{
    entries = ([ ]);
}

void register_name(string name)
{
    set_object_name("KV:stores:" + name);
}

void put(string key, mixed value)
{
    entries[key] = value;
    writes++;
}

mixed get(string key)
{
    return entries[key];
}

int size()
{
    return map_sizeof(entries);
}

int query_writes()
{
    return writes;
}

void wipe()
{
    entries = ([ ]);
}
```

The one inherit is `/lib/util/named` ([kernel-libraries.md](kernel-libraries.md)): `set_object_name` registers the clone in the platform's name index, and `find_named` resolves it from anywhere, O(1), no reference held. Also add the new compile to `src/usr/KV/initd.c` so future cold boots build it:

```c
    compile_object("obj/store");
    compile_object("sys/kv_daemon");
```

## 3. Growing the live daemon

The daemon gains four jobs: minting named stores, the tagged write (next section), the audit observer (the section after), and the gated wipe (the finale). Replace `src/usr/KV/sys/kv_daemon.c` with the grown form:

```c
/*
 * The KV service, grown into a coordinator: the flat store from the
 * first tutorial, plus named store clones, a tag index maintained in
 * the same atomic write as the entry, an audit observer fed by a
 * dispatched event property, and a capability-gated wipe.
 */

# include <kernel/kernel.h>

inherit "/lib/util/named";
inherit properties "/lib/util/properties";

# define STORE_PROG	"/usr/KV/obj/store"
# define MERRY		"/usr/Merry/sys/merry"
# define CAPABILITYD	"/kernel/sys/capabilityd"
# define ADMIN_CAPABILITY	"kv:admin"
# define EVENT_PROP	"kv:last-event"
# define AUDIT_PROP	"kv:audit-log"

private mapping store;      /* the flat store: key : value */
private int counter;        /* the rollback demo's counter */
private mapping byTag;      /* tag : ({ "store:key", ... }) */
private mapping stores;     /* name : store clone */

static void create()
{
    properties::create();
    store = ([ ]);
    byTag = ([ ]);
    stores = ([ ]);
    set_property(AUDIT_PROP, ({ }));
}

/*
 * one-time migration for the live daemon: the recompile added these
 * variables, and they arrive nil on existing state. patch() is the
 * platform's migration hook; here you drive it by hand once.
 */
void patch()
{
    if (!byTag) {
	properties::create();
	byTag = ([ ]);
	stores = ([ ]);
	set_property(AUDIT_PROP, ({ }));
    }
}

/* ---- the flat store, unchanged from the first tutorial ---- */

void put(string key, mixed value)
{
    store[key] = value;
}

mixed get(string key)
{
    return store[key];
}

void remove(string key)
{
    store[key] = nil;
}

int query_counter()
{
    return counter;
}

atomic void increment_and_fail()
{
    counter++;
    error("deliberate failure after mutating counter");
}

int size()
{
    return map_sizeof(store);
}

/* ---- named stores: the second object kind ---- */

object create_store(string name)
{
    object st;

    if (stores[name]) {
	return stores[name];
    }
    st = clone_object(STORE_PROG);
    st->register_name(name);
    stores[name] = st;
    return st;
}

object query_store(string name)
{
    return find_named("KV:stores:" + name);
}

/* ---- the tagged write: entry, index, and event in one commit ---- */

atomic void put_tagged(string storename, string key, mixed value,
		       string tag)
{
    object st;

    st = find_named("KV:stores:" + storename);
    if (!st) {
	error("no such store: " + storename);
    }
    st->put(key, value);
    if (!byTag[tag]) {
	byTag[tag] = ({ });
    }
    byTag[tag] += ({ storename + ":" + key });
    set_property(EVENT_PROP, "put " + storename + ":" + key +
		 " tag=" + tag);
}

atomic void put_tagged_and_fail(string storename, string key,
				mixed value, string tag)
{
    put_tagged(storename, key, value, tag);
    error("deliberate failure after entry, index, and event");
}

string *tagged(string tag)
{
    return byTag[tag] ? byTag[tag] : ({ });
}

/* ---- the audit observer: armed in code, fed by the dispatcher ---- */

void arm_audit()
{
    MERRY->register_observer(this_object(), EVENT_PROP, "main",
	"Set($this, \"" + AUDIT_PROP + "\", " +
	"Get($this, \"" + AUDIT_PROP + "\") + ({ $new })); " +
	"return TRUE;");
}

string *audit_log()
{
    return query_raw_property(AUDIT_PROP);
}

/* ---- the capability-gated admin operation ---- */

atomic int wipe_all(string principal)
{
    string *names;
    int i;

    if (!principal ||
	!CAPABILITYD->is_allowed(ADMIN_CAPABILITY, principal)) {
	return -1;
    }
    store = ([ ]);
    byTag = ([ ]);
    names = map_indices(stores);
    for (i = 0; i < sizeof(names); i++) {
	stores[names[i]]->wipe();
    }
    set_property(EVENT_PROP, "wipe by " + principal);
    return 1;
}
```

Three structural points before you compile. The daemon becomes a **property host** (`inherit properties "/lib/util/properties"`), because the dispatcher fires observers on *property* writes -- and a property host must chain `properties::create()`, or the first `set_property` dies with `Index on bad type`. The recompile **adds variables** to a live object, and new variables arrive `nil` on existing state; `patch()` is the platform's migration hook for exactly this, and with one daemon you drive it by hand once (at scale, the upgrade cascade drives it for every instance: [common-tasks.md](common-tasks.md) Migrate live state after a data-shape change; note the cascade's `upgrade -p` verb lives on a registered System login, not the `admin` console, which answers it `No command`). And the daemon needs no System-auto inherit -- `clone_object` and `find_named` are all the reach it uses.

Compile the new kind, recompile the daemon, migrate, and mint the first store -- a fresh console login, so history starts at `$0`:

```text
# compile /usr/KV/obj/store.c
$0 = </usr/KV/obj/store>
# compile /usr/KV/sys/kv_daemon.c
$1 = </usr/KV/sys/kv_daemon>
# code "/usr/KV/sys/kv_daemon"->patch()
$2 = nil
# code "/usr/KV/sys/kv_daemon"->get("lang")
$3 = "LPC"
# code "/usr/KV/sys/kv_daemon"->create_store("main")
$4 = </usr/KV/obj/store#301>
# code "/usr/KV/sys/kv_daemon"->arm_audit()
$5 = nil
```

`$3` is the quiet win: the recompile replaced the program and the store you re-seeded survived it, exactly as in the first tutorial. `$4` is the new kind existing: a clone, with its own number (yours will differ), registered as `KV:stores:main`. `$5` armed the audit observer -- in code, from the domain's own daemon, which is one of the three shapes the dispatcher's registrar gate admits (an object may register observers on targets in its own domain: [observers.md](observers.md) Registration).

## 4. The tagged write: entry, index, and event in one commit

`put_tagged` writes three things: the entry into the store clone, the index row into `byTag`, and the event property. All three sit in one `atomic` function, which is the platform's answer to the question every database-shaped system answers with locks or reconciliation jobs: **the index can never disagree with the store**, because they commit or roll back together -- across two objects ([application-authoring.md](application-authoring.md) Modeling domain data states the pattern; here it spans the daemon and the clone).

Give the wire face the new routes. In `src/usr/WWW/obj/server.c`, replace `dispatch()` once more -- the grown form keeps every route you had and adds three:

```c
private void dispatch(HttpRequest request, StringBuffer body)
{
    string method, path, key, storename, tag, token, reply, subject;
    string *hits;
    mixed value, auth;
    object st;
    int i;

    method = request->method();
    path = request->path();

    if (method == "GET" && path == "/health") {
	emit(makeResponse(200, "OK", "ok\n"), "ok\n");
	return;
    }

    if (method == "GET" && sscanf(path, "/kv/tagged/%s", tag) != 0 &&
	strlen(tag) != 0) {
	hits = "/usr/KV/sys/kv_daemon"->tagged(tag);
	reply = "";
	for (i = 0; i < sizeof(hits); i++) {
	    reply += hits[i] + "\n";
	}
	if (reply == "") {
	    reply = "none\n";
	}
	emit(makeResponse(200, "OK", reply), reply);
	return;
    }

    if (method == "DELETE" && path == "/kv") {
	auth = request->headerValue("Authorization");
	if (typeof(auth) == T_OBJECT) {
	    auth = auth->transport();
	}
	subject = nil;
	if (typeof(auth) == T_STRING &&
	    sscanf(auth, "Bearer %s", token) != 0) {
	    subject = "/usr/System/sys/authd"->validate(token);
	}
	if (!subject) {
	    emit(makeResponse(401, "Unauthorized", "a valid session token is required\n"),
		 "a valid session token is required\n");
	    return;
	}
	if ("/usr/KV/sys/kv_daemon"->wipe_all(subject) < 0) {
	    emit(makeResponse(403, "Forbidden", "kv:admin required\n"),
		 "kv:admin required\n");
	    return;
	}
	emit(makeResponse(200, "OK", "wiped\n"), "wiped\n");
	return;
    }

    if (sscanf(path, "/kv/%s/%s/%s", storename, key, tag) == 3 &&
	method == "PUT") {
	"/usr/KV/sys/kv_daemon"->put_tagged(storename, key,
					    drainBody(body), tag);
	emit(makeResponse(200, "OK", "stored\n"), "stored\n");
	return;
    }

    if (sscanf(path, "/kv/%s/%s", storename, key) == 2 &&
	method == "GET") {
	st = "/usr/KV/sys/kv_daemon"->query_store(storename);
	if (!st || st->get(key) == nil) {
	    emit(makeResponse(404, "Not Found", "no such entry\n"),
		 "no such entry\n");
	} else {
	    reply = (string) st->get(key) + "\n";
	    emit(makeResponse(200, "OK", reply), reply);
	}
	return;
    }

    if (sscanf(path, "/kv/%s", key) != 0 && strlen(key) != 0) {
	if (method == "GET") {
	    value = "/usr/KV/sys/kv_daemon"->get(key);
	    if (value == nil) {
		emit(makeResponse(404, "Not Found", "no such key\n"),
		     "no such key\n");
	    } else {
		reply = (string) value + "\n";
		emit(makeResponse(200, "OK", reply), reply);
	    }
	    return;
	}
	if (method == "PUT") {
	    "/usr/KV/sys/kv_daemon"->put(key, drainBody(body));
	    emit(makeResponse(200, "OK", "stored\n"), "stored\n");
	    return;
	}
	if (method == "DELETE") {
	    "/usr/KV/sys/kv_daemon"->remove(key);
	    emit(makeResponse(200, "OK", "removed\n"), "removed\n");
	    return;
	}
    }

    emit(makeResponse(404, "Not Found", "404 Not Found\n"),
	 "404 Not Found\n");
}
```

(The `DELETE /kv` block is the finale's admin route; it compiles now and refuses until section 7 provisions the authority. Route order matters: the literal and deeper paths parse before the flat `/kv/%s` catch-all, and DGD's `sscanf` `%s` stops at the next literal, so `/kv/main/greeting/demo` splits into three.)

Recompile the server (`compile /usr/WWW/obj/server.c` at the console -- `$6` if your session matches), then drive the composition from a second terminal:

```sh
curl http://localhost:8080/kv/lang
# LPC

curl -X PUT --data-binary 'hello from the composition' http://localhost:8080/kv/main/greeting/demo
# stored

curl http://localhost:8080/kv/main/greeting
# hello from the composition

curl -X PUT --data-binary 'a second entry' http://localhost:8080/kv/main/note/demo
# stored

curl http://localhost:8080/kv/tagged/demo
# main:greeting
# main:note
```

The flat store and the named store answer side by side, and the tag index found both entries without walking anything: the write site maintained it.

## 5. The audit observer and the rollback

Each `put_tagged` also wrote the event property, and the observer you armed appended it to the audit log -- synchronously, inside the same commit. Read the trail, then try to corrupt it:

```text
# code "/usr/KV/sys/kv_daemon"->audit_log()
$0 = ({ "put main:greeting tag=demo", "put main:note tag=demo" })
# code "/usr/KV/sys/kv_daemon"->put_tagged_and_fail("main", "ghost", "never", "demo")
Error: deliberate failure after entry, index, and event.
# code "/usr/KV/sys/kv_daemon"->audit_log()
$1 = ({ "put main:greeting tag=demo", "put main:note tag=demo" })
# code "/usr/KV/sys/kv_daemon"->tagged("demo")
$2 = ({ "main:greeting", "main:note" })
```

`put_tagged_and_fail` wrote the entry, the index row, the event -- and the observer appended the audit line -- and then the error rolled *all four* back, the reaction included. No ghost entry, no ghost index row, no ghost audit line. This is the first tutorial's atomicity demonstration grown to its real size: reactions fire inside the write's envelope, so an aborted write cannot leave its audit behind ([signal-applications.md](signal-applications.md); [dispatcher.md](dispatcher.md)).

## 6. The persistence win, composed

The same shutdown as every tutorial before, now over the whole composition. `reboot` at the console, then restore and drive the wire with no login at all:

```sh
/path/to/dgd/bin/dgd example.dgd state/snapshot
```

```sh
curl http://localhost:8080/kv/main/greeting
# hello from the composition

curl http://localhost:8080/kv/tagged/demo
# main:greeting
# main:note

curl http://localhost:8080/kv/lang
# LPC
```

The clone with its entries, the index, the audit trail, the observer's registration, and the flat store all came back from one image. Nothing was serialized, and nothing needed to be.

## 7. The admin surface: a capability-gated wipe

The wipe route is live but refuses everyone. Authority on this platform arrives in three layers ([identity.md](identity.md) The three-layer authorization split), and a whole-store wipe is the middle one: a **platform capability**, granted by an operator to a principal, checked at the daemon's choke point -- `wipe_all` asks `capabilityd->is_allowed("kv:admin", principal)` and refuses without it. Provision the authority at the console:

```text
# identity mint 1
identity: minted identity:59999d48-7b24-4b99-aa0c-b995fe776ce5 with 1 recovery codes
identity: code 1: MO9IUVAXtxRarxhLAz7l
identity: store the codes now; only their hashes are kept
# identity grant 59999d48-7b24-4b99-aa0c-b995fe776ce5 kv:admin
identity: granted kv:admin
# session mint identity:59999d48-7b24-4b99-aa0c-b995fe776ce5
session: minted for identity:59999d48-7b24-4b99-aa0c-b995fe776ce5
session: token DRNsyFGz7ITDufO-CqIQQM7oDivpqy7ygEhttBycxbE
session: present the token now; only its hash is kept
```

(Your uuid, code, and token will differ; each is printed exactly once, and only hashes persist -- a statedump cannot leak them.) Three verbs, three pieces: an identity record, the capability bound to its principal, and a bearer session proving it on the wire. Now drive the route through its three answers:

```sh
curl -X DELETE http://localhost:8080/kv
# a valid session token is required

curl -X DELETE -H "Authorization: Bearer <token>" http://localhost:8080/kv
# wiped

curl http://localhost:8080/kv/main/greeting
# no such entry
curl http://localhost:8080/kv/lang
# no such key
curl http://localhost:8080/kv/tagged/demo
# none
```

And the discrimination that makes it real -- a *valid* session whose identity was never granted the capability (mint a second identity and a session for it, skip the grant) answers differently:

```sh
curl -X DELETE -H "Authorization: Bearer <ungranted-token>" http://localhost:8080/kv
# kv:admin required
```

No token is 401; a proven identity without the grant is 403; the granted identity wipes. The route never parses authority itself: the transport proves *who* (authd validates the token to a subject), and the daemon's choke point decides *may they* (`is_allowed`). That separation is the platform's authorization doctrine at tutorial scale, and [application-authoring.md](application-authoring.md) Give your application an operator surface is the decision menu for when you build your own.

## What you just used

| Section | Piece | Depth |
|---|---|---|
| 1 | Extensions as cold-boot facts; the restore precondition | [operations.md](operations.md) |
| 2 | Clones as the entity kind; logical names | [application-authoring.md](application-authoring.md) Modeling domain data |
| 3 | Live-object migration: `patch()` and the upgrade cascade | [common-tasks.md](common-tasks.md); [code-lifecycle.md](code-lifecycle.md) |
| 4 | A secondary index that cannot disagree with its store | [application-authoring.md](application-authoring.md) Modeling domain data |
| 5 | Observers inside the atomic envelope; rollback un-happens reactions | [dispatcher.md](dispatcher.md); [signal-applications.md](signal-applications.md) |
| 6 | Orthogonal persistence over a composed domain | [persistence.md](persistence.md) |
| 7 | The three-layer authorization split, at the choke point | [identity.md](identity.md); [capability.md](capability.md) |

## Cleaning up

The same collision warning as the last tutorial, plus one more: any harness run removes `src/usr/WWW`, and `src/usr/KV` is yours to remove (`common-tasks.md` Reset a development checkout to a clean slate). The minted identity and session live in the image and expire or reset with it.

## Where to next

- **[composite-applications.md](composite-applications.md)**: the same seams at full scale -- WebAuthn ceremonies instead of operator-minted sessions, an inventory instead of a KV store, event streams on the observer you just armed. You have now built every mechanism it composes; read it in its stages.
- **[application-authoring.md](application-authoring.md)** Give your application an operator surface: the decision menu behind section 7's choice.
- **[capability.md](capability.md)**: the authority model behind `is_allowed` -- principals, the store, the grant paths, and what the model deliberately does not claim.
- **[identity.md](identity.md)**: the identity substrate the console verbs drove -- records, credentials, sessions, and the agent story this tutorial did not need.
