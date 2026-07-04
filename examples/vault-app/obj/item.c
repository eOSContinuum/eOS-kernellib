/*
 * Bare property-bearing clonable: the property lib plus naming, and
 * nothing else -- no schema of its own, no marshaling overrides. Its
 * marshaling shape is the default every property-bearing object
 * receives (queryStateRoot "Core:Entries"), which the test driver uses
 * to prove the property-table round-trip that needs no per-app schema.
 */

inherit "/lib/util/properties";
inherit "/lib/util/named";
