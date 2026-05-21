/*
 * Merry script clonable.
 *
 * Each instance holds the source LPC, the AST tuple (srcarr / obarr /
 * constmap), and a pointer to the compiled /merry/<md5> program. The
 * constructor parses Merry source via SYS_MERRY, generates wrapper LPC
 * from the AST, hashes it, and compiles to a cached object.
 *
 * Lifted from SkotOS /usr/SkotOS/data/merry.c per LM-2 sub-decisions.
 * Changes:
 *   - VAL_SAM cases removed from expand_to_source + expand_to_lpc
 *     per LM-2 sub-decision (c).
 *   - samarr per-script storage + womble_merry samarr cleanup removed.
 *   - Generate_pcdata SAM-rendering helper + /usr/XML/lib/xmlgen
 *     inherit removed.
 *   - SkotOS configure(string lpc) merged into create(string lpc) per
 *     cloud-server _F_init dispatch (L14 #10 from LV-4.5d).
 *   - MERRYNODE(n) replaced with MERRY_NODE(n) via <Merry.h>.
 *   - /lib/string -> /lib/util/ascii; /lib/womble inherit removed
 *     (samarr was its only consumer; merrynode wombles args itself).
 */

# include <status.h>
# include <type.h>
# include <Merry.h>
# include "/usr/Merry/include/merry.h"

private inherit "/lib/util/ascii";
private inherit "/lib/util/lpc";
private inherit hex "/lib/util/hex";

string source, hash;
mixed  *srcarr;
object program;

mapping constmap;
object *obarr;

object query_program();
object get_program();
string expand_to_lpc(mixed *arr);

# define ST_VIRGIN	1
# define ST_STRING	2
# define ST_QUOTE	3
# define ST_COMMENT	4
# define ST_OBJREF	10
# define ST_ARGREF	11
# define ST_ARGREF_P	12
# define ST_SLEEP	13

string query_dtd_type() { return "merry"; }

static
void create(string lpc) {
   int    i ,sz;
   string result, *constants;
   mixed  *res;

   if (lpc == nil) {
      /* plain create() with no args; nothing to configure */
      return;
   }

   lpc = strip(lpc);
   res = MERRY->parse_merry(lpc);

   srcarr   = res[0];
   obarr    = res[1];
   constmap = res[2];

   /*
    * Generate LPC from Merry code:
    */
   result = expand_to_lpc(srcarr);

   if (!constmap) {
      constmap = ([ ]);
   }

   constants = map_indices(constmap);
   for (i = 0, sz = sizeof(constants); i < sz; i ++) {
      constants[i] = "# define " + constants[i] + " " +
	 expand_to_lpc(constmap[constants[i]]);
   }

   source =
      "inherit \"/usr/Merry/lib/merrynode\";\n" +
      "# include \"/usr/Merry/include/merrynode.h\"\n" +
      implode(constants, "\n") + "\n" +
      "mixed merry(string mode, string signal, string label) {\n" +
      "    switch(label) { case \"virgin\": {\n" + result + "}\n" +
      "    }\n" +
      "}\n";

   hash = hex::format(hash_string("MD5", source));

   get_program();	/* force the compile */
}

object query_program() { return program; }	/* for debugging */
mixed *query_srcarr() { return srcarr; }	/* for debugging */

private
int is_simple(string str) {
   int i;

   for (i = 0; i < strlen(str); i ++) {
      switch(str[i]) {
      case 'a'..'z': case 'A'..'Z': case '_': case '0'..'9':
	 break;
      default:
	 return FALSE;
      }
   }
   return TRUE;
}

void convert_sleep(mixed *bit);

private
string fixprop(string str) {
   string lc;
   int bad, i;

   if (strlen(str) && ((str[0] >= 'a' && str[0] <= 'z') || (str[0] >= 'A' && str[0] <= 'Z'))) {
      for (i = 1; !bad && i < strlen(str); i ++) {
	 switch(str[i]) {
	 case 'a' .. 'z':
	 case 'A' .. 'Z':
	 case '0' .. '9':
	 case '_':
	    break;
	 default:
	    bad = TRUE;
	 }
      }
   } else {
      bad = TRUE;
   }
   if (bad) {
      return "\"" + str + "\"";
   }
   return str;
}

private string trim_line(string line)
{
	int sz;

	sz = strlen(line);

	while (sz > 0 && line[sz - 1] == ' ') {
		sz--;
	}

	return line[0 .. sz - 1];
}

string expand_to_source(mixed arr) {
   string *result;
   mixed bit;
   int i;

   if (typeof(arr) != T_ARRAY) {
      return "\n/*\nInternal error expanding source, value is not an array." +
      "\nActual value: " + dumpValue(arr) + "\n*/";
   }

   result = allocate(sizeof(arr));
   for (i = 0; i < sizeof(arr); i ++) {
      bit = arr[i];
      if (typeof(bit) == T_ARRAY) {
	 switch(bit[0]) {
	 case VAL_OBJREF:
	    if (obarr[bit[1]-1] == nil) {
	       /* object was destructed */
	       result[i] = "nil /* defunct */";
	    } else {
	       result[i] = "${" + object_name(obarr[bit[1]-1]) + "}";
	    }
	    break;
	 case VAL_ARGREF:
	    if (is_simple(bit[1])) {
	       result[i] = "$" + bit[1];
	    } else {
	       result[i] = "$(" + bit[1] + ")";
	    }
	    break;
	 case VAL_SLEEP:
	    convert_sleep(bit);
	    if (sizeof(bit) == 3) {
	       result[i] = "$delay(" + expand_to_source(bit[2]) +
		  ", FALSE, " + expand_to_source(bit[1]) + ");";
	    } else {
	       result[i] = "$delay(" + expand_to_source(bit[2]) +
		  ", " + expand_to_source(bit[3]) + ", " +
		  expand_to_source(bit[1]) + ");";
	    }
	    break;
	 case VAL_ARGLIST: {
	    string *tmp;
	    int j;

	    tmp = allocate(sizeof(bit)-1);
	    for (j = 1; j < sizeof(bit); j ++) {
	       if (is_simple(bit[j][0])) {
		  tmp[j-1] = "$" + bit[j][0] + ": " +
		     expand_to_source(bit[j][1]);
	       } else {
		  tmp[j-1] = "$(" + bit[j][0] + "): " +
		     expand_to_source(bit[j][1]);
	       }
	    }
	    result[i] = implode(tmp, ", ");
	    break;
	 }
	 case VAL_PROPGET:
	    result[i] = expand_to_source(bit[1]) + "." + fixprop(bit[2]);
	    break;
	 case VAL_PROPSET:
	    result[i] = expand_to_source(bit[1]) + "." + fixprop(bit[2]) +
	       " = " + expand_to_source(bit[3]);
	    break;
	 case VAL_PROPMOD:
	    result[i] = expand_to_source(bit[1]) + "." + fixprop(bit[2]) +
	       " " + bit[3] + "= " + expand_to_source(bit[4]);
	    break;
	 case VAL_PROPPOSTFIX:
	    result[i] = expand_to_source(bit[1]) + "." + fixprop(bit[2]) +
	       bit[3];
	    break;
	 case VAL_PROPPREFIX:
	    result[i] = bit[3] + expand_to_source(bit[1]) + "." +
	       fixprop(bit[2]);
	    break;
	 case VAL_CONSTANT:
	    result[i] = "constant " + bit[1] + " = " +
	       expand_to_source(constmap[bit[1]]) + ";";
	    break;
	 case VAL_LABELCALL: {
	    string *tmp;
	    int j;

	    tmp = allocate(sizeof(bit)-3);
	    for (j = 3; j < sizeof(bit); j ++) {
	       if (is_simple(bit[j][0])) {
		  tmp[j-3] = "$" + bit[j][0] + ": " +
		     expand_to_source(bit[j][1]);
	       } else {
		  tmp[j-3] = "$(" + bit[j][0] + "): " +
		     expand_to_source(bit[j][1]);
	       }
	    }
	    result[i] = (bit[1] ? bit[1] : "") + "::" + bit[2] + "(" +
	       implode(tmp, ", ") + ")";
	    break;
	 }
	 case VAL_LABELREF:
	    result[i] = bit[1] + "::";
	    break;
	 }
      } else {
	 result[i] = bit;
      }
   }

   {
      string src;
      string *lines;
      int sz;

      src = implode(result, "");

      lines = explode(src, "\n");

      /* trim whitespace off end of line */
      for (sz = sizeof(lines); --sz >= 0; ) {
         lines[sz] = trim_line(lines[sz]);
      }

      sz = sizeof(lines);

      /* trim blank lines off end */
      while (sz > 0 && lines[sz - 1] == "") {
         sz--;
      }

      return implode(lines[0 .. sz - 1], "\n");
   }

   return implode(result, "");
}

string query_source() {
   return expand_to_source(srcarr);
}

mixed evaluate(mixed args...) { return get_program()->evaluate(args...); }


void convert_sleep(mixed *bit) {
   if (sizeof(bit) == 4 && typeof(bit[3]) == T_STRING) {
      /* backwards compatible conversion */
      bit[3] = ({ bit[3] });
   }
   if (typeof(bit[2]) == T_STRING) {
      /* backwards compatible conversion */
      bit[2] = ({ bit[2] });
   }
   if (typeof(bit[1]) == T_STRING) {
      if (bit[1][0] != '\"') {
	 /* backwards compatible conversion */
	 bit[1] = "\"" + bit[1] + "\"";
      }
      bit[1] = ({ bit[1] });
   }
}

string expand_to_lpc(mixed *arr) {
   string *result;
   mixed bit;
   int i;

   result = allocate(sizeof(arr));

   for (i = 0; i < sizeof(arr); i ++) {
      bit = arr[i];
      if (typeof(bit) == T_ARRAY) {
	 switch(bit[0]) {
	 case VAL_OBJREF:
	    result[i] = "obref(" + (bit[1]-1) + ")";
	    break;
	 case VAL_ARGREF:
	    result[i] = "(args[\"" + lower_case(bit[1]) + "\"])";
	    break;
	 case VAL_SLEEP: {
	    string label, ret;

	    convert_sleep(bit);

	    if (sizeof(bit) == 3) {
	       ret = "FALSE";
	    } else {
	       ret = expand_to_lpc(bit[3]);
	    }
	    label = expand_to_lpc(bit[1]);
	    result[i] = "{ do_delay(mode, signal, " +
	       expand_to_lpc(bit[2]) + ", " + label + "); return " +
	       ret + "; case " + label + ": ; } ";
	    break;
	 }
	 case VAL_ARGLIST: {
	    string *tmp;
	    int j;

	    tmp = allocate(sizeof(bit)-1);
	    for (j = 1; j < sizeof(bit); j ++) {
	       tmp[j-1] = "\"" + bit[j][0] + "\", " + expand_to_lpc(bit[j][1]);
	    }
	    result[i] = "({ " + implode(tmp, ", ") + " })";
	    break;
	 }
	 case VAL_PROPGET:
	    result[i] = "Get(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\")";
	    break;
	 case VAL_PROPSET:
	    result[i] = "Set(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\", " +
	       expand_to_lpc(bit[3]) + ")";
	    break;
	 case VAL_PROPMOD:
	    result[i] = "Set(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\", Get(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\") " + bit[3][0 .. 0] + " " +
	       expand_to_lpc(bit[4]) + ")";
	    break;
	 case VAL_PROPPOSTFIX:
	 case VAL_PROPPREFIX:
	    result[i] = "Set(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\", Get(" + expand_to_lpc(bit[1]) +
	       ", \"" + bit[2] + "\") " + bit[3][0 .. 0] + " 1)";
	    break;
	 case VAL_CONSTANT:
	    result[i] = "";	/* top of file */
	    break;
	 case VAL_LABELCALL: {
	    string *tmp;
	    int j;

	    tmp = allocate(sizeof(bit)-3);
	    for (j = 3; j < sizeof(bit); j ++) {
	       tmp[j-3] = "\"" + bit[j][0] + "\", " + expand_to_lpc(bit[j][1]);
	    }
	    if (bit[1]) {
	       result[i] = "LabelCall(\"" + lower_case(bit[1]) + "\", \"" +
		  lower_case(bit[2]) + "\", " +
		  "({ " + implode(tmp, ", ") + " }))";
	    } else {
	       result[i] = "Call(this, \"" + lower_case(bit[2]) + "\", " +
		  "({ " + implode(tmp, ", ") + " }))";
	    }
	    break;
	 }
	 case VAL_LABELREF:
	    result[i] = "LabelRef(\"" + bit[1] + "\")";
	    break;
	 default:
	    error("unknown merry nugget type: " + dumpValue(bit));
	 }
      } else if (typeof(bit) == T_STRING) {
	 result[i] = bit;
      } else {
	 error("bad type of merry nugget: " + dumpValue(bit));
      }
   }
   return implode(result, "");
}

#include "denew.h"

atomic
object get_program() {
   if (!source) {
      /*
       * Fixing already existing Merry nodes, hopefully.
       */
      int    i, sz;
      string result, *constants;

      /*
       * Generate LPC from Merry code:
       */
      result = expand_to_lpc(srcarr);

      if (!constmap) {
	 constmap = ([ ]);
      }

      constants = map_indices(constmap);
      for (i = 0, sz = sizeof(constants); i < sz; i ++) {
	 constants[i] = "# define " + constants[i] + " " +
	    expand_to_lpc(constmap[constants[i]]);
      }

      source =
	 "inherit \"/usr/Merry/lib/merrynode\";\n" +
	 "# include \"/usr/Merry/include/merrynode.h\"\n" +
	 implode(constants, "\n") + "\n" +
	 "mixed merry(string mode, string signal, string label) {\n" +
	 "    switch(label) { case \"virgin\": {\n" + result + "}\n" +
	 "    }\n" +
	 "}\n";

      hash = hex::format(hash_string("MD5", source));
   }

   if (!program) {
      string name;

      if (!hash) {
	 error("Internal error: Missing MD5 hash for Merry program");
      }

      name    = MERRY_NODE(hash);
      program = find_object(name);

      if (!program) {
	 if (!source) {
	    error("Internal error: Missing source for Merry program");
	 }

	 program = compile_object(name, denew(source));
	 MERRY->new_merry_node(program);
      }
   }

   /* Configure the object with the right constants. */
   program->set_object_array(obarr);
   return program;
}

mixed
query_property(string prop) {
   if (prop && sscanf(lower_case(prop), "merry:%s", prop)) {
      if (prop == "source") {
	 return query_source();
      }
      error("unknown merry: property");
   }
   error("only merry: properties served here");
}
