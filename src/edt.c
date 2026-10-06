#include "edt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tbx.h"

#ifndef EDT_VERSION
#define EDT_VERSION "dev"
#endif

enum {
    STEPS_KEY = 1000000,  /* a key moves some state around */
    STEPS_DRAW = 4000000, /* a paint of a wide hex view visits every byte shown */
};

static edt *editor(filo_ctx *ctx) {
    return app_of(ctx)->user;
}

static tbuf *buf_of(filo_ctx *ctx) {
    return &editor(ctx)->tb;
}

/* ---- the builtins that are this host's ---- */

static int b_tb_path(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-path takes no argument");
    }
    *out = filo_cstring(editor(ctx)->path);
    return FILO_OK;
}

/* Writes the text beside the file and renames it over, so a save that
   fails halfway leaves the old file whole; the old file's permissions go
   to the new one. "" when it worked, the system's words when not. */
static const char *save_file(const char *path, const uint8_t *data, size_t len) {
    char tmp[EDT_PATH_MAX + 16];
    int w = snprintf(tmp, sizeof(tmp), "%s.edt-save", path);
    if (w < 0 || (size_t)w >= sizeof(tmp)) {
        return "File name too long";
    }
    struct stat st;
    bool had = stat(path, &st) == 0;
    if (had && S_ISDIR(st.st_mode)) {
        return "Is a directory";
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return strerror(errno);
    }
    size_t put = len > 0 ? fwrite(data, 1, len, f) : 0;
    int err = put == len ? 0 : errno;
    if (fflush(f) != 0 && err == 0) {
        err = errno;
    }
    if (err == 0 && fsync(fileno(f)) != 0) {
        err = errno;
    }
    if (fclose(f) != 0 && err == 0) {
        err = errno;
    }
    if (err == 0 && had) {
        (void)chmod(tmp, st.st_mode & 07777U);
    }
    if (err == 0 && rename(tmp, path) != 0) {
        err = errno;
    }
    if (err != 0) {
        (void)unlink(tmp);
        return strerror(err);
    }
    return "";
}

/* (tb-save) or (tb-save "path"): "" when saved, else why not. Saving under
   a new name keeps it. */
static int b_tb_save(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    edt *e = editor(ctx);
    if (n > 1 || (n == 1 && a[0].kind != FILO_STRING)) {
        return filo_fail(ctx, "tb-save expects at most a path");
    }
    char path[EDT_PATH_MAX];
    if (n == 1) {
        if (a[0].u.str.len == 0 || a[0].u.str.len >= sizeof(path)) {
            *out = filo_cstring("File name too long");
            return FILO_OK;
        }
        memcpy(path, a[0].u.str.ptr, a[0].u.str.len);
        path[a[0].u.str.len] = '\0';
    } else if (e->path[0] == '\0') {
        *out = filo_cstring("No such file or directory"); /* a new file with no name */
        return FILO_OK;
    } else {
        memcpy(path, e->path, strlen(e->path) + 1);
    }
    const char *why = save_file(path, e->tb.text, e->tb.len);
    if (why[0] == '\0') {
        memcpy(e->path, path, strlen(path) + 1);
        e->tb.dirty = false;
    }
    *out = filo_cstring(why);
    return FILO_OK;
}

/* ---- the pager over the editor ---- */

static void pager_on_key(void *ctx, uint32_t cp) {
    edt *e = ctx;
    if (pager_key(&e->pg, &e->a.t, cp)) {
        return;
    }
    pager_hide(&e->a.t);
    e->paging = false;
    (void)term_app_leave(&e->a.t);
    app_repaint(&e->a);
}

static void pager_on_resize(void *ctx) {
    edt *e = ctx;
    pager_resize(&e->pg, &e->a.t);
}

static void pager_on_tick(void *ctx, uint32_t ms) {
    (void)ctx;
    (void)ms;
}

static const term_app pager_app = {
    .on_key = pager_on_key,
    .on_resize = pager_on_resize,
    .on_tick = pager_on_tick,
};

static void to_pager(void *user, const uint8_t *data, size_t n) {
    pager_load(user, data, n);
}

static bool markdown(const char *name) {
    size_t n = strlen(name);
    if (n <= 3) {
        return false;
    }
    return strcmp(name + n - 3, ".md") == 0;
}

/* The text, or the file at file when it is not NULL, rendered into the
   pager, which takes the screen over the editor until q or Esc. The run
   that asked ends first: the editor paints only when it is on top. */
static void show(edt *e, const char *name, const char *file) {
    pager_reset(&e->pg, name[0] != '\0' ? name : "(new)");
    e->render.emit = to_pager;
    e->render.user = &e->pg;
    e->render.base = ""; /* no site behind a desktop's files */
    e->render.slug = NULL;
    md_reset(&e->render, markdown(name));
    if (file == NULL) {
        md_feed(&e->render, e->tb.text, e->tb.len);
    } else {
        FILE *f = fopen(file, "rb");
        if (f != NULL) {
            uint8_t chunk[16 * 1024];
            size_t got = sizeof(chunk);
            while (got == sizeof(chunk)) { /* a short read is the end, or a failure */
                got = fread(chunk, 1, sizeof(chunk), f);
                md_feed(&e->render, chunk, got);
            }
            (void)fclose(f);
        }
    }
    md_end(&e->render);
    if (!term_app_enter(&e->a.t, &pager_app, e)) {
        return;
    }
    e->paging = true;
    pager_show(&e->pg, &e->a.t);
}

/* (pager-buffer): the text, as the pager shows a file of its name —
   markdown rendered when it is one — once this key is handled. */
static int b_pager_buffer(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "pager-buffer takes no argument");
    }
    edt *e = editor(ctx);
    show(e, e->path, NULL);
    *out = filo_bool(true);
    return FILO_OK;
}

/* (pager-file path): "" when the file is about to be shown, else why not. */
static int b_pager_file(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "pager-file expects a path");
    }
    char path[EDT_PATH_MAX];
    if (a[0].u.str.len == 0 || a[0].u.str.len >= sizeof(path)) {
        *out = filo_cstring("No such file or directory");
        return FILO_OK;
    }
    memcpy(path, a[0].u.str.ptr, a[0].u.str.len);
    path[a[0].u.str.len] = '\0';
    struct stat st;
    if (stat(path, &st) != 0) {
        *out = filo_cstring(strerror(errno));
        return FILO_OK;
    }
    if (S_ISDIR(st.st_mode)) {
        *out = filo_cstring("Is a directory");
        return FILO_OK;
    }
    show(editor(ctx), path, path);
    *out = filo_cstring("");
    return FILO_OK;
}

/* True when the file at path holds exactly data: an edit undone by hand
   leaves nothing to save. A file that is not there holds nothing. */
static bool same_as_file(const char *path, const uint8_t *data, size_t len) {
    struct stat st;
    if (path[0] == '\0' || stat(path, &st) != 0) {
        return len == 0;
    }
    if ((uint64_t)st.st_size != len) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    uint8_t chunk[64 * 1024];
    size_t at = 0;
    while (at < len) {
        size_t got = fread(chunk, 1, sizeof(chunk), f);
        if (got == 0 || got > len - at || memcmp(chunk, data + at, got) != 0) {
            break;
        }
        at += got;
        if (got < sizeof(chunk)) {
            break; /* the end of the file, or a failed read */
        }
    }
    (void)fclose(f);
    return at == len;
}

static bool saved(filo_ctx *ctx) {
    const edt *e = editor(ctx);
    return same_as_file(e->path, e->tb.text, e->tb.len);
}

/* The builtins that are edt's, past the app's: the text buffer, saving,
   and the pager. */
static bool extend(app *a, filo_ctx *ctx) {
    (void)a;
    tbx_register(ctx, buf_of, saved);
    (void)filo_register_builtin(ctx, "tb-path", b_tb_path);
    (void)filo_register_builtin(ctx, "tb-save", b_tb_save);
    (void)filo_register_builtin(ctx, "pager-buffer", b_pager_buffer);
    /* a full table refuses the last ones first: this one standing means
       they all do */
    return filo_register_builtin(ctx, "pager-file", b_pager_file) == FILO_OK;
}

const app_spec app_program = {"edt", EDT_VERSION, false, STEPS_KEY, STEPS_KEY, STEPS_DRAW, extend};

void edt_input(edt *e, const uint8_t *data, size_t n) {
    app_input(&e->a, data, n);
}

void edt_tick(edt *e, uint32_t ms) {
    app_tick(&e->a, ms);
}

void edt_resize(edt *e, uint16_t cols, uint16_t rows) {
    app_resize(&e->a, cols, rows);
}

bool edt_done(const edt *e) {
    return app_done(&e->a);
}

const char *edt_rescue(const edt *e, char *where, size_t cap) {
    int w = snprintf(where, cap, "%s.edt-rescue", e->path[0] != '\0' ? e->path : "edt");
    if (w < 0 || (size_t)w >= cap) {
        return "File name too long";
    }
    return save_file(where, e->tb.text, e->tb.len);
}

/* ---- starting ---- */

static bool fail(char *why, size_t cap, const char *a, const char *b) {
    (void)snprintf(why, cap, "%s%s", a, b);
    return false;
}

/* The file into the buffer: a new one when there is none, refused when it
   is a directory or does not fit. */
static bool load(edt *e, const char *path, char *why, size_t cap) {
    if (strlen(path) >= sizeof(e->path)) {
        return fail(why, cap, path, ": File name too long");
    }
    memcpy(e->path, path, strlen(path) + 1);
    tb_init(&e->tb);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return true; /* a new file, written on the first save */
        }
        return fail(why, cap, path, ": cannot be read");
    }
    if (S_ISDIR(st.st_mode)) {
        return fail(why, cap, path, ": Is a directory");
    }
    if ((uint64_t)st.st_size > TB_CAP) {
        return fail(why, cap, path, ": File too large");
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return fail(why, cap, path, ": cannot be read");
    }
    uint8_t *data = malloc((size_t)st.st_size + 1);
    if (data == NULL) {
        (void)fclose(f);
        return fail(why, cap, path, ": not enough memory");
    }
    size_t got = fread(data, 1, (size_t)st.st_size + 1, f); /* one more: grown since stat */
    (void)fclose(f);
    bool ok = false;
    if (got <= TB_CAP) {
        ok = tb_load(&e->tb, data, got);
    }
    free(data);
    if (!ok) {
        return fail(why, cap, path, ": File too large");
    }
    return true;
}

bool edt_start(edt *e, const uint8_t *fbb, size_t fbb_len, const char *path, uint16_t cols,
               uint16_t rows, char *why, size_t cap) {
    e->paging = false;
    e->a.user = e;
    if (!load(e, path, why, cap)) {
        return false;
    }
    return app_start(&e->a, &app_program, fbb, fbb_len, cols, rows, why, cap);
}
