/*
 * Index subsystem initialization daemon.
 *
 * Compiles the second-level naming registry: a logical-name -> object
 * mapping distinct from DGD's path-based object_name() / find_object().
 * Callers set logical names via /lib/util/named.c::set_object_name(),
 * which delegates to Index->set_name(); reverse lookup is via
 * Index->query_object(name) (also wrapped as
 * /lib/util/named.c::find_named()).
 *
 * The Index daemon also maintains an object -> name reverse map so that
 * kernel-level destruct (via the /kernel/lib/auto.c hook) can clear an
 * object's name registration without the destruct-site needing to know
 * what the name was.
 */

void create()
{
    compile_object("sys/index_daemon");
}
