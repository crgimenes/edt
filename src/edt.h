#ifndef EDT_H
#define EDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "md.h"
#include "pager.h"
#include "tbuf.h"

/* edt: a text and hex editor whose face is a Filo program (the .filo files
   of prog/, compiled to edt.fbb) and whose text lives in C. This is the
   core, on filo-term's app: no terminal, no files but the one being edited
   and the ones the pager is asked to show. Bytes come in (edt_input),
   bytes for the terminal go out through the term, and a test drives it
   the same way a tty does. */

enum {
    EDT_PATH_MAX = 1024,
};

typedef struct {
    app a;

    tbuf tb;
    char path[EDT_PATH_MAX]; /* where a save goes; "" for a new file */

    /* The pager over the editor, pushed by a pager-* builtin. */
    pager pg;
    md render;
    bool paging;
} edt;

/* The program as bytecode: edt.fbb, a bundle whose one member, "edt", has
   an entry for each .filo file of prog/ ("draw" for draw.filo). The build
   makes it with the filo CLI (Go's or C's: the bytes are the same) and the
   binary embeds it (tools/embed.sh); the same file loads in any Filo VM
   that has what it imports. */
extern const uint8_t edt_fbb[];
extern const size_t edt_fbb_len;

/* Opens path (a new file when there is none; "" for one with no name yet)
   at cols x rows with the program fbb (edt.fbb, which must outlive e) and
   paints. False with why when it cannot: a directory, a file too large, a
   program that does not load or start. */
bool edt_start(edt *e, const uint8_t *fbb, size_t fbb_len, const char *path, uint16_t cols,
               uint16_t rows, char *why, size_t cap);

void edt_input(edt *e, const uint8_t *data, size_t n);
void edt_tick(edt *e, uint32_t ms);
void edt_resize(edt *e, uint16_t cols, uint16_t rows);
bool edt_done(const edt *e);

/* Writes the unsaved text beside the file, as <path>.edt-rescue, for when
   the terminal goes away mid-edit. "" when written, else why not. */
const char *edt_rescue(const edt *e, char *where, size_t cap);

#endif
