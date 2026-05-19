/*
 * file-system convenience helpers atop DGD file kfuns.
 *
 * paveWay:         create all directories leading up to a given full path
 * writeDataToFile: append a streaming-data object's chunks to a file
 * copyFile:        atomic chunked file copy via .copying temp + atomic rename
 *
 * Calls resolve through the auto chain (read_file / write_file / remove_file
 * / rename_file / get_dir / make_dir kfuns wrapped with permission checks).
 */

/*
 * create all directories leading up to a given full path
 */
static void paveWay(string path)
{
    string *bits;
    int i;

    bits = explode(path, "/");
    for (i = 0; i < sizeof(bits); i++) {
	path = "/" + implode(bits[.. i - 1], "/");
	if (sizeof(get_dir(path)[1]) == 0) {
	    make_dir(path);
	}
    }
}

/*
 * write the chunks of a streaming-data object to a file via repeated
 * write_file
 */
static void writeDataToFile(string file, object data)
{
    string *chunks;
    int i;

    chunks = data->query_chunks();
    for (i = 0; i < sizeof(chunks); i++) {
	write_file(file, chunks[i]);
    }
}

/*
 * atomic chunked file copy: writes to <dst>.copying then renames into place
 */
static void copyFile(string src, string dst)
{
    mixed **info;
    int sz, ix;

    info = get_dir(src);
    if (sizeof(info[0]) != 1) {
	error("source file does not exist: " + src);
    }
    sz = info[1][0];

    remove_file(dst + ".copying");
    catch {
	while (ix + 0x8000 < sz) {
	    write_file(dst + ".copying", read_file(src, ix, 0x8000), ix);
	    ix += 0x8000;
	}
	write_file(dst + ".copying", read_file(src, ix), ix);
    } : {
	remove_file(dst + ".copying");
	error("copy failed");
    }
    remove_file(dst);
    rename_file(dst + ".copying", dst);
}
