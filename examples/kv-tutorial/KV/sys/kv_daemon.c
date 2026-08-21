private mapping store;      /* the key-value store */
private int counter;        /* a running count, for the rollback demo */

static void create()
{
    store = ([ ]);
}

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

int size()
{
    return map_sizeof(store);
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
