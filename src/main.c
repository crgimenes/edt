/* edt on a POSIX terminal: filo-term's tty does the terminal, and what is
   edt's here is the file — the one to open, and the rescue when the
   terminal goes away with changes not saved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edt.h"
#include "tty.h"

static const char usage[] = "usage: edt [+LINE] [FILE]\n"
                            "\n"
                            "Edits FILE as text, or byte by byte as hex (Esc X switches). A file\n"
                            "that does not exist is created on the first save; with no FILE, the\n"
                            "first save asks for a name. +LINE opens on that line, as vi does.\n"
                            "\n"
                            "Keys: Esc the menu; ^S save  ^Q quit  ^F find  ^G next for hands\n"
                            "that know them.\n"
                            "\n"
                            "Example: edt +120 notes.txt\n";

static edt E; /* the buffer is megabytes: not for the stack */

static void say(const char *a, const char *b, const char *c, const char *d) {
    fputs(a, stderr);
    fputs(b, stderr);
    fputs(c, stderr);
    fputs(d, stderr);
    fputs("\n", stderr);
}

/* +N, digits only: a file named "+" or "+x" is still a file. */
static bool line_arg(const char *s, size_t *line) {
    if (s[0] != '+' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(s + 1, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *line = (size_t)v;
    return true;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fputs(usage, stdout);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        fputs("edt ", stdout);
        fputs(app_program.version, stdout);
        fputs("\n", stdout);
        return 0;
    }
    const char *path = "";
    size_t line = 0;
    for (int i = 1; i < argc; i++) {
        if (line_arg(argv[i], &line)) {
            continue;
        }
        if (path[0] != '\0') {
            fputs(usage, stderr);
            return 2;
        }
        path = argv[i];
    }
    char why[512];
    uint16_t cols = 0;
    uint16_t rows = 0;
    if (!tty_open(&cols, &rows, why, sizeof(why))) {
        say("edt: ", why, "", "");
        return 1;
    }
    if (!edt_start(&E, edt_fbb, edt_fbb_len, path, line, cols, rows, why, sizeof(why))) {
        tty_close();
        say(why, "", "", "");
        return 1;
    }
    bool finished = tty_run(&E.a);
    tty_close();
    if (finished || !E.tb.dirty) {
        return 0;
    }
    char where[EDT_PATH_MAX + 16];
    const char *failed = edt_rescue(&E, where, sizeof(where));
    if (failed[0] != '\0') {
        say("edt: changes lost, cannot write ", where, ": ", failed);
        return 1;
    }
    say("edt: closed with changes not saved; they are in ", where, "", "");
    return 1;
}
