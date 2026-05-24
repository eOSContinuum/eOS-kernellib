/*
 * Merry AST node-type tags.
 *
 * Lifted from SkotOS /usr/SkotOS/include/merry.h. VAL_SAM removed
 * per LM-2 sub-decision (c) (drop $"" SAM-token surface entirely).
 * Remaining tags renumbered to keep the sequence contiguous.
 */

# define VAL_OBJREF		0
# define VAL_ARGREF		1
# define VAL_SLEEP		2
# define VAL_ARGLIST		3
# define VAL_PROPGET		4
# define VAL_PROPSET		5
# define VAL_PROPMOD		6
# define VAL_PROPPOSTFIX	7
# define VAL_PROPPREFIX		8
# define VAL_CONSTANT		9
# define VAL_LABELCALL		10
# define VAL_LABELREF		11
