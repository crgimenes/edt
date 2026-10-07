/* edt on a POSIX terminal: filo-term's tty does the terminal, and what is
   edt's here is the file — the one to open, and the rescue when the
   terminal goes away with changes not saved. */
#include <stdio.h>
#include <string.h>

#include "edt.h"
#include "tty.h"

static const char usage[] = "usage: edt [FILE]\n"
                            "\n"
                            "Edits FILE as text, or byte by byte as hex (Esc X switches). A file\n"
                            "that does not exist is created on the first save; with no FILE, the\n"
                            "first save asks for a name.\n"
                            "\n"
                            "Keys: ^S save  ^Q quit  ^F find  ^G next  Esc the menu of the rest.\n"
                            "\n"
                            "Example: edt notes.txt\n";

static edt E; /* the buffer is megabytes: not for the stack */

static void say(const char *a, const char *b, const char *c, const char *d) {
    fputs(a, stderr);
    fputs(b, stderr);
    fputs(c, stderr);
    fputs(d, stderr);
    fputs("\n", stderr);
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
    if (argc > 2) {
        fputs(usage, stderr);
        return 2;
    }
    char why[512];
    uint16_t cols = 0;
    uint16_t rows = 0;
    if (!tty_open(&cols, &rows, why, sizeof(why))) {
        say("edt: ", why, "", "");
        return 1;
    }
    if (!edt_start(&E, edt_fbb, edt_fbb_len, argc == 2 ? argv[1] : "", cols, rows, why,
                   sizeof(why))) {
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
