// Replaces the global operator new/delete of uc.exe with mimalloc: the guest thread allocates and frees small blocks all the time (display lists, state
// snapshots, draw packets) and mimalloc's per-thread free lists are faster than the CRT heap. Linked into the executable only.
#include <mimalloc-new-delete.h>
