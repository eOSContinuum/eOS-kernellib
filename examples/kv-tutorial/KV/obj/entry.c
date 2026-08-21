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
