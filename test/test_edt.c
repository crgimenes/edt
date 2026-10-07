/* The editor driven as a terminal drives it — bytes in — with what it
   painted and what it wrote to disk checked afterwards. Files live in a
   directory of their own under the system's temporary one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "edt.h"
#include "keys.h"
#include "utf8.h"

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static edt E; /* megabytes: not for the stack */
static char dir[256];

static const char *path_of(const char *name) {
    static char p[512];
    (void)snprintf(p, sizeof(p), "%s/%s", dir, name);
    return p;
}

static void write_file(const char *name, const void *data, size_t n) {
    FILE *f = fopen(path_of(name), "wb");
    if (f == NULL) {
        printf("cannot write %s\n", path_of(name));
        exit(2);
    }
    (void)fwrite(data, 1, n, f);
    (void)fclose(f);
}

static size_t read_file(const char *name, uint8_t *dst, size_t cap) {
    FILE *f = fopen(path_of(name), "rb");
    if (f == NULL) {
        return (size_t)-1;
    }
    size_t n = fread(dst, 1, cap, f);
    (void)fclose(f);
    return n;
}

static bool start(const char *name, uint16_t cols, uint16_t rows) {
    char why[256];
    bool ok = edt_start(&E, edt_fbb, edt_fbb_len, path_of(name), cols, rows, why, sizeof(why));
    if (!ok) {
        printf("  start: %s\n", why);
    }
    return ok;
}

static void keys(const char *s) {
    edt_input(&E, (const uint8_t *)s, strlen(s));
    edt_tick(&E, 1000); /* a lone ESC is told apart by time */
}

/* Row y of what the program painted, as UTF-8. */
static const char *row_text(uint16_t y) {
    static char buf[4096];
    size_t n = 0;
    for (uint16_t x = 0; x < E.a.target.cols && n + 5 < sizeof(buf); x++) {
        uint32_t cp = E.a.target.cells[y][x].cp;
        if (cp != 0) {
            n += utf8_encode((uint8_t *)buf + n, cp);
        }
    }
    buf[n] = '\0';
    return buf;
}

static bool screen_has(const char *needle) {
    for (uint16_t y = 0; y < E.a.target.rows; y++) {
        if (strstr(row_text(y), needle) != NULL) {
            return true;
        }
    }
    return false;
}

static bool starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* Esc, then X: apart in time, or the two read as Alt-X. */
static void toggle(void) {
    keys("\x1b");
    keys("x");
}

static void dump(void) {
    for (uint16_t y = 0; y < E.a.target.rows; y++) {
        printf("  |%s|\n", row_text(y));
    }
}

#define CTRL(c) ((char)((c) - 64))

static void test_text_edit_and_save(void) {
    write_file("a.txt", "hello\nworld\n", 12);
    CHECK(start("a.txt", 80, 24));
    CHECK(screen_has("hello"));
    CHECK(screen_has("Line 1  Col 1"));
    keys("X");
    CHECK(screen_has("Xhello"));
    CHECK(screen_has("a.txt *"));
    const char save[2] = {CTRL('S'), 0};
    keys(save);
    CHECK(screen_has("Saved"));
    uint8_t got[64];
    size_t n = read_file("a.txt", got, sizeof(got));
    CHECK(n == 13 && memcmp(got, "Xhello\nworld\n", 13) == 0);
    struct stat st;
    CHECK(stat(path_of("a.txt.edt-save"), &st) != 0); /* the save's scratch is gone */
}

/* A rune past 0xFFFF (an emoji) is typed like any other, not taken for one
   of the key codes of the private use area. */
static void test_types_emoji(void) {
    write_file("e.txt", "", 0);
    CHECK(start("e.txt", 80, 24));
    keys("\xf0\x9f\x98\x80!");
    const char save[2] = {CTRL('S'), 0};
    keys(save);
    uint8_t got[16];
    size_t n = read_file("e.txt", got, sizeof(got));
    CHECK(n == 5 && memcmp(got, "\xf0\x9f\x98\x80!", 5) == 0);
}

static void test_new_file_and_quit(void) {
    (void)unlink(path_of("new.txt"));
    CHECK(start("new.txt", 80, 24));
    keys("abc");
    const char quit[2] = {CTRL('Q'), 0};
    keys(quit);
    CHECK(screen_has("Save changes?"));
    CHECK(!edt_done(&E));
    keys("y");
    CHECK(edt_done(&E));
    uint8_t got[16];
    CHECK(read_file("new.txt", got, sizeof(got)) == 3 && memcmp(got, "abc", 3) == 0);
}

static void test_no_file_asks_for_a_name(void) {
    char why[256];
    CHECK(edt_start(&E, edt_fbb, edt_fbb_len, "", 200, 24, why,
                    sizeof(why))); /* wide: the path fits */
    CHECK(screen_has("(new)"));
    keys("draft");
    const char save[2] = {CTRL('S'), 0};
    keys(save);
    CHECK(screen_has("Save as:"));
    keys(path_of("named.txt"));
    keys("\r");
    uint8_t got[16];
    CHECK(read_file("named.txt", got, sizeof(got)) == 5 && memcmp(got, "draft", 5) == 0);
    CHECK(!edt_done(&E));
    CHECK(screen_has("Saved ") && screen_has("named.txt"));
    CHECK(!E.tb.dirty);
}

/* Quitting an unnamed text: yes to saving asks for the name, and saving
   under it ends the editor; Esc at the name keeps it open. */
static void test_no_file_quit_saves_then_ends(void) {
    char why[256];
    CHECK(edt_start(&E, edt_fbb, edt_fbb_len, "", 80, 24, why, sizeof(why)));
    keys("x");
    const char quit[2] = {CTRL('Q'), 0};
    keys(quit);
    keys("y");
    CHECK(screen_has("Save as:"));
    keys("\x1b");
    CHECK(!edt_done(&E));
    keys(quit);
    keys("y");
    keys(path_of("left.txt"));
    keys("\r");
    CHECK(edt_done(&E));
    uint8_t got[16];
    CHECK(read_file("left.txt", got, sizeof(got)) == 1 && got[0] == 'x');
}

/* A change undone by hand is no change: no star, no question on the way out. */
static void test_undone_edit_is_clean(void) {
    write_file("u.txt", "same\n", 5);
    CHECK(start("u.txt", 80, 24));
    keys("Z");
    CHECK(screen_has("u.txt *"));
    keys("\x7f");
    CHECK(!screen_has("u.txt *"));
    toggle();
    keys("6"); /* 's' (73) becomes 'c' (63) */
    CHECK(screen_has("u.txt *"));
    keys("\x1b[D7"); /* back, and 7 again */
    CHECK(!E.tb.dirty);
    const char quit[2] = {CTRL('Q'), 0};
    keys(quit);
    CHECK(edt_done(&E));
}

/* Esc U, apart in time as Esc X is. */
static void undo(void) {
    keys("\x1b");
    keys("u");
}

static void test_undo(void) {
    write_file("z.txt", "one\n", 4);
    CHECK(start("z.txt", 80, 24));
    undo();
    CHECK(screen_has("Nothing to undo"));
    keys("\x1b[F"); /* End */
    keys(" two\rthree");
    keys("\x7f\x7f");
    CHECK(screen_has("thr"));
    undo();
    CHECK(screen_has("three"));
    undo();
    CHECK(!screen_has("thr") && screen_has("one two"));
    CHECK(screen_has("Line 2  Col 1"));
    undo();
    CHECK(!screen_has("two") && !screen_has("z.txt *"));
    CHECK(screen_has("Line 1  Col 4"));

    toggle();
    keys("\x1b[H"); /* Home: the row's first byte */
    keys("4");      /* half a byte: 'o' (6F) is 4F */
    undo();
    CHECK(starts_with(row_text(0), "00000000  6F 6E 65 0A"));
    keys("41");
    CHECK(starts_with(row_text(0), "00000000  41 6E 65 0A")); /* whole bytes after the undo */
    undo();
    CHECK(starts_with(row_text(0), "00000000  6F 6E 65 0A"));
    CHECK(!screen_has("z.txt *"));
}

static void test_hex_view_shows_bytes(void) {
    write_file("b.bin", "hello\x01\xFF", 7);
    CHECK(start("b.bin", 80, 24));
    toggle();
    CHECK(starts_with(row_text(0), "00000000  68 65 6C 6C 6F 01 FF"));
    CHECK(strstr(row_text(0), "hello\xe2\x98\xba\xc2\xa0") != NULL); /* ☺ and CP437's 0xFF */
    CHECK(screen_has("68h 104 01101000"));
    CHECK(screen_has("00000000/00000007"));
    keys("\x1b[C\x1b[C");
    CHECK(screen_has("00000002/00000007  6Ch 108 01101100"));
    toggle();
    CHECK(screen_has("hello"));
    CHECK(screen_has("Line 1  Col 3"));
}

static void test_hex_typing_nibbles_and_chars(void) {
    write_file("c.bin", "hello", 5);
    CHECK(start("c.bin", 80, 24));
    toggle();
    keys("4");
    CHECK(starts_with(row_text(0), "00000000  48 65")); /* the high half first */
    keys("1");
    CHECK(starts_with(row_text(0), "00000000  41 65"));
    CHECK(screen_has("00000001/00000005")); /* and on to the next byte */
    keys("\t");                             /* to the characters' side */
    keys("Zq");
    CHECK(starts_with(row_text(0), "00000000  41 5A 71 6C 6F"));
    keys("\x1b[F\x1b[C"); /* End, then past the last byte: typing adds one */
    keys("\t");
    keys("7e");
    const char save[2] = {CTRL('S'), 0};
    keys(save);
    uint8_t got[16];
    CHECK(read_file("c.bin", got, sizeof(got)) == 6 && memcmp(got, "AZqlo~", 6) == 0);
}

static void test_goto_and_find_bytes(void) {
    uint8_t data[64];
    for (int i = 0; i < 64; i++) {
        data[i] = (uint8_t)i;
    }
    write_file("d.bin", data, sizeof(data));
    CHECK(start("d.bin", 80, 24));
    toggle();
    const char go[2] = {CTRL('O'), 0};
    keys(go);
    keys("0x2A\r");
    CHECK(screen_has("0000002A/00000040"));
    keys(go);
    keys("16\r");
    CHECK(screen_has("00000010/00000040"));
    const char find[2] = {CTRL('F'), 0};
    keys(find);
    keys("30 31 32\r");
    CHECK(screen_has("00000030/00000040"));
    keys(find);
    keys("ff\r");
    CHECK(screen_has("Not found: ff"));
}

/* A file of every byte, opened and saved without an edit, comes back byte
   for byte: nothing is decoded or normalised on the way. */
static void test_binary_round_trip(void) {
    uint8_t data[512];
    for (int i = 0; i < 512; i++) {
        data[i] = (uint8_t)((i * 37) + 11);
    }
    write_file("e.bin", data, sizeof(data));
    CHECK(start("e.bin", 80, 24));
    toggle();
    keys("\x1b[6~\x1b[6~\x1b[1;5F"); /* PgDn twice, Ctrl-End */
    CHECK(screen_has("00000200/00000200"));
    keys(":"); /* not a command in the hex view: types nothing on the hex side */
    CHECK(!E.tb.dirty);
    keys("\x1b"); /* Esc: the menu */
    keys("s");
    uint8_t got[600];
    CHECK(read_file("e.bin", got, sizeof(got)) == 512 && memcmp(got, data, 512) == 0);
}

static void test_narrow_terminal(void) {
    write_file("f.bin", "0123456789", 10);
    CHECK(start("f.bin", 40, 10));
    toggle();
    CHECK(starts_with(row_text(0), "00000000  30 31 32 33  0123"));
    CHECK(starts_with(row_text(1), "00000004  34 35 36 37  4567"));
}

static void test_paste_paints_once(void) {
    write_file("g.txt", "", 0);
    CHECK(start("g.txt", 80, 24));
    keys("\x1b[200~pasted text\x1b[201~");
    CHECK(screen_has("pasted text"));
}

static void test_rescue_keeps_unsaved_text(void) {
    write_file("h.txt", "kept\n", 5);
    CHECK(start("h.txt", 80, 24));
    keys("new ");
    char where[1100];
    CHECK(edt_rescue(&E, where, sizeof(where))[0] == '\0');
    CHECK(strcmp(where, path_of("h.txt.edt-rescue")) == 0);
    uint8_t got[16];
    CHECK(read_file("h.txt.edt-rescue", got, sizeof(got)) == 9 &&
          memcmp(got, "new kept\n", 9) == 0);
    CHECK(read_file("h.txt", got, sizeof(got)) == 5); /* the file itself untouched */
}

/* ^P shows the text in the pager, markdown rendered for a .md; q or Esc
   gives the screen back to the text as it was. */
static void test_preview_in_the_pager(void) {
    write_file("p.md", "# Title\n\nsome *text*\n", 22);
    CHECK(start("p.md", 80, 24));
    keys("\x10"); /* ^P */
    CHECK(E.paging);
    CHECK(E.pg.text_len > 0 && memmem(E.pg.text, E.pg.text_len, "Title", 5) != NULL);
    CHECK(memmem(E.pg.text, E.pg.text_len, "# Title", 7) == NULL); /* rendered, not raw */
    keys("q");
    CHECK(!E.paging);
    CHECK(screen_has("# Title"));
    keys("\x1b");
    keys("m"); /* the menu's M too */
    CHECK(E.paging);
    keys("\x1b");
    CHECK(!E.paging && screen_has("Line 1  Col 1"));
}

/* Help opens the manual of what is edited, when the VM has it; a desktop
   without rocchetto's Redcode page says so and stays in the text. */
static void test_manual_where_there_is_none(void) {
    write_file("w.red", "mov 0, 1\n", 9);
    CHECK(start("w.red", 80, 24));
    keys("\x1b");
    keys("h");
    CHECK(!E.paging);
    CHECK(screen_has("edt: no manual here: No such file or directory"));
}

/* The colors the program gives each class tb-spans reports. */
enum { FG_COMMENT = 244, FG_STRING = 108, FG_NUMBER = 139, FG_KEYWORD = 74 };

static int fg_at(uint16_t row, uint16_t col) {
    return (int)E.a.target.cells[row][col].fg;
}

/* The text is colored by its extension's language, a comment that spans
   lines included, and an edit above a line recolors it. */
static void test_highlight_by_language(void) {
    const char c[] = "int x = 42; // hi\nchar *s = \"str\";\n/* open\n   still */ int y;\n";
    write_file("a.c", c, sizeof(c) - 1);
    CHECK(start("a.c", 80, 24));
    CHECK(fg_at(0, 0) == FG_KEYWORD && fg_at(0, 4) == CV_COLOR_DEFAULT);
    CHECK(fg_at(0, 8) == FG_NUMBER && fg_at(0, 12) == FG_COMMENT && fg_at(0, 16) == FG_COMMENT);
    CHECK(fg_at(1, 10) == FG_STRING && fg_at(1, 14) == FG_STRING &&
          fg_at(1, 15) == CV_COLOR_DEFAULT);
    CHECK(fg_at(2, 0) == FG_COMMENT && fg_at(3, 3) == FG_COMMENT && fg_at(3, 12) == FG_KEYWORD);
    keys("/* ");
    CHECK(fg_at(0, 3) == FG_COMMENT && fg_at(1, 0) == FG_COMMENT && fg_at(3, 0) == FG_COMMENT);
    CHECK(fg_at(3, 12) == FG_KEYWORD); /* the comment closed there still */
    keys("\x7f\x7f\x7f");
    CHECK(fg_at(0, 0) == FG_KEYWORD && fg_at(1, 0) == FG_KEYWORD);
    keys("\x1b[1;2C\x1b[1;2C\x1b[1;2C"); /* a selection keeps its colors */
    CHECK((E.a.target.cells[0][1].attr & CV_A_REV) != 0 && fg_at(0, 1) == FG_KEYWORD);
    CHECK((E.a.target.cells[0][4].attr & CV_A_REV) == 0);

    const char filo[] = "; note\n(def str-len-x \"a\nb\" 5)\n";
    write_file("b.filo", filo, sizeof(filo) - 1);
    CHECK(start("b.filo", 80, 24));
    CHECK(fg_at(0, 0) == FG_COMMENT && fg_at(1, 1) == FG_KEYWORD);
    CHECK(fg_at(1, 9) == CV_COLOR_DEFAULT); /* the len inside str-len-x is a name's */
    CHECK(fg_at(1, 15) == FG_STRING && fg_at(2, 0) == FG_STRING && fg_at(2, 3) == FG_NUMBER);

    const char red[] = "        MOV 0, 1 ; imp\n";
    write_file("w.red", red, sizeof(red) - 1);
    CHECK(start("w.red", 80, 24));
    CHECK(fg_at(0, 8) == FG_KEYWORD && fg_at(0, 12) == FG_NUMBER && fg_at(0, 17) == FG_COMMENT);

    /* marks made further down before an edit above them are not trusted */
    static char many[700 * 7 + 1];
    for (size_t i = 0; i < 700; i++) {
        /* lines laid end to end in a zeroed buffer: no terminator wanted */
        // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
        memcpy(many + (i * 7), "int x;\n", 7);
    }
    write_file("m.c", many, sizeof(many) - 1);
    CHECK(start("m.c", 80, 24));
    for (int i = 0; i < 40; i++) {
        keys("\x1b[6~");
    }
    CHECK(fg_at(0, 0) == FG_KEYWORD);
    for (int i = 0; i < 40; i++) {
        keys("\x1b[5~");
    }
    keys("/*");
    for (int i = 0; i < 40; i++) {
        keys("\x1b[6~");
    }
    CHECK(fg_at(0, 0) == FG_COMMENT);

    write_file("notes.txt", "int 42\n", 7); /* no language: plain */
    CHECK(start("notes.txt", 80, 24));
    CHECK(fg_at(0, 0) == CV_COLOR_DEFAULT && fg_at(0, 4) == CV_COLOR_DEFAULT);
}

static void test_refusals(void) {
    char why[256];
    CHECK(!edt_start(&E, edt_fbb, edt_fbb_len, dir, 80, 24, why, sizeof(why)));
    CHECK(strstr(why, "Is a directory") != NULL);
}

/* Esc R lays a .filo out as filofmt does, the cursor on the same character
   of the code, and :fmt does the same; a source that does not close is left
   alone, and a file that is not Filo has neither. */
static void test_reformat_filo(void) {
    const char *src = "(def  f (fn (x)\n\n  (* x x)))";
    write_file("f.filo", src, strlen(src));
    CHECK(start("f.filo", 80, 24));
    keys("\x1b[B");
    keys("\x1b[B");
    keys("\x1b[C\x1b[C\x1b[C"); /* on the * */
    CHECK(screen_has("Line 3  Col 4"));
    keys("\x1b");
    CHECK(screen_has("Reformat"));
    keys("r");
    CHECK(screen_has("formatted"));
    CHECK(screen_has("    (* x x)))"));
    keys("\x1b[D"); /* a key clears the message: the position shows */
    keys("\x1b[C");
    CHECK(screen_has("Line 4  Col 6"));
    const char save[2] = {CTRL('S'), 0};
    keys(save);
    uint8_t got[128];
    const char *want = "(def f\n  (fn (x)\n\n    (* x x)))\n";
    size_t n = read_file("f.filo", got, sizeof(got));
    CHECK(n == strlen(want) && memcmp(got, want, n) == 0);
    keys("\x1b");
    keys(":");
    keys("fmt\r");
    CHECK(screen_has("formatted"));
    CHECK(!screen_has("f.filo *")); /* laid out already: nothing changed */

    const char *open = "(def g (fn (x) \"a)\n";
    write_file("g.filo", open, strlen(open));
    CHECK(start("g.filo", 80, 24));
    keys("\x1b");
    keys("r");
    CHECK(screen_has("edt: fmt: a string or a paren does not close"));
    CHECK(!screen_has("g.filo *"));

    write_file("t.txt", "(a  b)\n", 7);
    CHECK(start("t.txt", 80, 24));
    keys("\x1b");
    CHECK(!screen_has("Reformat"));
    keys(":");
    keys("fmt\r");
    CHECK(screen_has("only Filo source (.filo) is formatted"));
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    (void)snprintf(dir, sizeof(dir), "%s/edt-test-XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (mkdtemp(dir) == NULL) {
        printf("cannot make a directory for the tests\n");
        return 2;
    }
    test_text_edit_and_save();
    test_types_emoji();
    test_new_file_and_quit();
    test_no_file_asks_for_a_name();
    test_no_file_quit_saves_then_ends();
    test_undone_edit_is_clean();
    test_undo();
    test_hex_view_shows_bytes();
    test_hex_typing_nibbles_and_chars();
    test_goto_and_find_bytes();
    test_binary_round_trip();
    test_narrow_terminal();
    test_paste_paints_once();
    test_rescue_keeps_unsaved_text();
    test_preview_in_the_pager();
    test_manual_where_there_is_none();
    test_refusals();
    test_highlight_by_language();
    test_reformat_filo();
    if (failures > 0) {
        dump();
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all edt tests passed\n");
    return 0;
}
