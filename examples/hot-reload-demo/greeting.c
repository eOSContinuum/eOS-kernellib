/*
 * Greeting master for the hot-reload demonstration.
 *
 * A single function `greet()` returning a string. The demonstration
 * is that a POST /compile request can replace this object's program
 * with new source at runtime, and the next GET /greet picks up the
 * new behavior without a DGD restart.
 *
 * The initial response below is what the cold-boot smoke step 1
 * expects. The smoke's step 2 POSTs new source returning a different
 * string; step 3 confirms the new string is returned.
 */

string greet()
{
    return "hello before recompile\n";
}
