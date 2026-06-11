/*
 * File-backed parser-runner: a subclass of /lib/util/parse that
 * allows the specification of a grammar filename during
 * initialization, making the parse_string() call even simpler.
 *
 */

inherit "/lib/util/parse";

private string grammar;
private string grammar_file;
private int grammar_stamp;

static
void create(string scratch, string file, varargs int debug) {
   ::create(scratch, debug);

   grammar_file = file;
   grammar_stamp = 0;
}

static
mixed *parse_string(string str, varargs int trees) {
   mixed *info;
   int stamp;

   info = file_info(grammar_file);
   if (!info) {
      error("grammar file not found");
   }
   stamp = info[1];
   if (stamp > grammar_stamp) {
      grammar_stamp = stamp;
      grammar = read_file(grammar_file);
   }
   return ::parse_string(grammar, str, trees);
}

void
reset_fileparse() {
   grammar = nil;
   grammar_stamp = 0;
}
