/*
 * The smallest property host that can carry a reaction.
 *
 * One inherit: /lib/util/properties. That is the entire requirement
 * for an object to participate in signal-on-property -- the property
 * store is both the observed state AND the place observer scripts are
 * bound (under merry:on:<path>:<timing> keys). No ur-hierarchy, no
 * logical name, no schema: those compose in when an application needs
 * inheritance, name lookup, or persistence-to-XML, and the other
 * bundled examples demonstrate each.
 */

# include <type.h>

inherit "/lib/util/properties";
