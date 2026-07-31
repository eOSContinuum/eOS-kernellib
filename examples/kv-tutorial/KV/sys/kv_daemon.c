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
