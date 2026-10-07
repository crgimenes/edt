# edt

A small terminal editor for text and bytes. Esc X switches between a text
view and a hex view in the style of PC Tools, Norton's Disk Editor and Hiew.
The text is colored by its extension's language — C, Go, JS, Lua, shell,
SQL, JSON, asm, Filo and Redcode — and plain for any other.

The screen and the keys are a [Filo](https://github.com/crgimenes/clang_filo)
program (the `.filo` files in `prog/`), compiled to bytecode as `edt.fbb`.
The `edt` binary is a Filo VM (no parser, no compiler) with that bytecode
embedded; the text buffer, the terminal and the file handling are its C.
The same `edt.fbb` loads in any Filo VM that provides the builtins it
imports (`filo dump edt.fbb` lists them); one that lacks any refuses it.

```
edt [FILE]
```

A file that does not exist is created on the first save. With no FILE, the
first save asks for a name. edt asks whether to save on the way out only
when the text differs from the file: an edit undone by hand leaves nothing
to save. edt needs a terminal. It runs on macOS, Linux and the BSDs (POSIX
termios only).

## Keys

Text view:

| Key | Action |
| --- | --- |
| ^S | save |
| ^Q | quit (asks to save changes) |
| ^F / ^G | find / find next |
| ^O | go to a line |
| ^L | bare screen (no status line) |
| ^P | preview in the pager: Markdown rendered for a `.md`, plain text otherwise; q or Esc comes back |
| Esc | menu: **S**ave, save **A**s, **Q**uit, **D**elete, **C**opy, **P**aste, **U**ndo, delete **L**ine, **F**ind, **N**ext, **G**o to, **X** hex view, **M**arkdown preview, **B**are, **H**elp (the manual of what is edited, where the VM has one), **R**eformat (a `.filo`, laid out as filofmt lays it out), `:` command, `/` find |

Shift+arrows select. ^X or Shift+Del cuts, Ctrl+Ins copies, Shift+Ins pastes
and ^Y deletes the line. Copy and cut also reach the terminal's clipboard
through OSC 52. Pasting from the terminal (bracketed paste) inserts the text
in one step.

Esc U undoes the last edit, and again the one before: typing a line at a
time, a run of Backspace or Delete at once, a paste, a cut or a replaced
selection whole. The cursor goes back to where the edit was. How far back it
reaches is a build's choice (`FT_CFG_TB_UNDO`): the desktop build keeps 8 MB
of history, enough to undo a change to a whole file; a small device keeps
less, and the oldest edits are forgotten first.

The `:` commands are `w`, `w NAME`, `q`, `q!`, `wq` / `x`, `fmt`, and a line
number.

Reformat moves only blanks: the cursor stays on the same character of the
code, and a source whose strings or parens do not close is left as it is.
Esc U undoes it in one step, as it undoes anything else.

Hex view:

```
00000000  23 69 6E 63 6C 75 64 65  20 22 65 64 74 2E 68 22  #include "edt.h"
00000010  0A 0A 23 69 6E 63 6C 75  64 65 20 3C 65 72 72 6E  ◙◙#include <errn
...
sample.c                          00000010/0000012C  0Ah 10 00001010
```

| Key | Action |
| --- | --- |
| arrows, PgUp/PgDn, Home/End | move by byte, row, page |
| Ctrl+Home / Ctrl+End | start / end of the file |
| Tab | switch between the hex and the character side |
| 0-9 a-f (hex side) | overwrite the byte, high nibble first |
| any character (character side) | overwrite the byte with that character |
| ^O | go to an offset: `0x4D`, `4Dh` or decimal |
| ^F | find text, or bytes written as hex pairs: `4d 5a` |
| Esc U | undo, as in the text view |
| Esc X | back to text |

The status line shows the offset, the size and the byte under the cursor in
hex, decimal and binary. Characters are drawn as CP437, so control bytes
show as their glyphs. Rows hold 16, 8 or 4 bytes depending on the terminal's
width. The hex view overwrites bytes and can append after the last one. It
does not insert or delete bytes in the middle of a file.

## Safety

A save writes `FILE.edt-save`, syncs it, keeps the original's permissions and
renames it over `FILE`. If the terminal goes away (SIGHUP, SIGTERM, or input
closed) with changes not saved, edt writes them to `FILE.edt-rescue` and says
so on stderr.

## Install

```
brew install crgimenes/tap/edt
```

installs edt. Or take a binary from the
[releases](https://github.com/crgimenes/edt/releases): macOS (universal, arm64
and x86_64) and Linux (static, amd64 and arm64), nothing else to install.

## Build

edt is built from its own sources plus two sibling checkouts:
[filo-term](https://github.com/crgimenes/filo-term) (the terminal, canvas,
text buffer, key decoder, pager and the builtins over them) and
[clang_filo](https://github.com/crgimenes/clang_filo). By default they are
looked for at `../filo-term` and `../clang_filo`:

```
make                     # ./edt and edt.fbb
make FILO_TERM=/path/to/filo-term FILO=/path/to/clang_filo
make install             # to /usr/local/bin; PREFIX=~/.local for elsewhere
make test                # the core, driven like a terminal, under ASan/UBSan
make qa                  # build, clang-format, test, clang-tidy, cppcheck
make dist                # the release binaries in dist/ (needs zig for Linux)
```

It builds with any C11 compiler and libc. `make qa` needs LLVM's
clang-format and clang-tidy (`LLVM=` points at them) and cppcheck.

Files up to 8 MB and terminals up to 512x200 (see `CFG` in the Makefile).

## License

MIT
