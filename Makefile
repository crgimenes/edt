# edt: the editor, its core tested without a terminal, and the QA gate.
CC ?= cc
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
# The libraries edt is made of live beside it: the terminal, the app core
# and its POSIX loop, the text buffer, the key decoder, the pager and the
# builtins over them in filo-term, and Filo. Point these elsewhere to build
# against other copies.
FILO_TERM ?= ../filo-term
FILO ?= ../clang_filo
# The compiler of the program: C's, built from FILO, unless another is
# named. Go's filo writes the same bytes (make FILO_CLI=filo).
FILO_CLI ?= $(FILO)/build/filo
CLI_DEP = $(filter $(FILO)/build/filo,$(FILO_CLI))

WARN = -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion -Wundef
# A desktop's sizes, not a small device's: files up to 8 MB (and a million lines),
# an undo that holds a whole file, terminals up to 512 by 200, and arenas to match.
CFG = -DFT_CFG_TB_CAP='(8*1024*1024)' -DFT_CFG_TB_LINES_MAX='(1024*1024)' \
	-DFT_CFG_TB_UNDO='(8*1024*1024)' -DFT_CFG_COLS_MAX=512 -DFT_CFG_ROWS_MAX=200 \
	-DAPP_CFG_PERSISTENT='(4U*1024U*1024U)' -DAPP_CFG_RUN='(8U*1024U*1024U)'
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
INC = -Isrc -I$(FILO_TERM)/src -I$(FILO)
# glibc and musl hide POSIX (fileno, fsync, cfmakeraw, mkdtemp) under a
# strict -std; the BSDs and macOS ignore the macro.
FLAGS = -std=c11 -D_DEFAULT_SOURCE $(WARN) $(CFG) $(INC) -DEDT_VERSION='"$(VERSION)"'

LIBS = $(addprefix $(FILO_TERM)/src/,term.c canvas.c utf8.c tbuf.c keyin.c paint.c field.c tbx.c pager.c md.c hl.c app.c)
LIBHDRS = $(wildcard $(FILO_TERM)/src/*.h)
FILOSRC = $(addprefix $(FILO)/,filo.c filo_math.c filo_strings.c filo_nolibc.c filo_fmt.c)
PROG = $(wildcard prog/*.filo)
CORE = src/edt.c $(LIBS) $(FILOSRC)
HDRS = src/edt.h $(LIBHDRS) $(FILO)/filo.h

PREFIX ?= /usr/local
# A release links everything it can: LDFLAGS=-static on Linux (musl); macOS
# has no static libc, and the binary needs nothing past libSystem anyway.
LDFLAGS ?=

.PHONY: all test fmt fmt-check tidy check qa smoke install dist clean

# Two products of one build: ./edt, which carries the VM and the program,
# and edt.fbb, the program alone for any Filo VM that has what it imports.
all: edt edt.fbb

$(FILO)/build/filo:
	$(MAKE) -C $(FILO) build/filo

# What the binary gives a bundle, listed by the binary's own code: the
# profile the program is compiled against and held to.
build/edt.vm: $(CORE) $(FILO_TERM)/tools/appvm.c $(HDRS)
	@mkdir -p build
	$(CC) -O1 $(FLAGS) -DFILO_VM_ONLY -o build/appvm $(CORE) $(FILO_TERM)/tools/appvm.c
	./build/appvm > $@

# One entry per file (draw.filo is "draw"), one member named by the unit's
# file: build/edt.fbc is "edt". Compiled against the profile, a call to a
# builtin of edt's is an import, as a call to the core's is; the check
# makes a name edt does not have fail the build, not the first keystroke.
edt.fbb: $(PROG) build/edt.vm $(CLI_DEP)
	$(FILO_CLI) build -vm build/edt.vm -o build/edt.fbc $(PROG)
	$(FILO_CLI) bundle -o $@ build/edt.fbc
	$(FILO_CLI) check -vm build/edt.vm $@

build/fbb.c: edt.fbb
	sh $(FILO_TERM)/tools/embed.sh edt_fbb edt.fbb > $@

# The binary is a VM: no parser, no compiler, only the bytecode it embeds.
edt: $(CORE) build/fbb.c src/main.c $(FILO_TERM)/src/tty.c $(HDRS)
	$(CC) -O2 $(FLAGS) -DFILO_VM_ONLY -o $@ $(CORE) build/fbb.c src/main.c $(FILO_TERM)/src/tty.c \
		$(LDFLAGS)

# The core under the sanitizers, driven the way a terminal drives it.
test: $(CORE) build/fbb.c test/test_edt.c $(HDRS)
	@mkdir -p build
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(FLAGS) -DFILO_VM_ONLY \
		-o build/test_edt $(CORE) build/fbb.c test/test_edt.c
	./build/test_edt

fmt:
	$(CLANG_FORMAT) -i src/*.c src/*.h test/*.c

fmt-check:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.h test/*.c

# The analyzer's insecureAPI check wants C11 Annex K (memcpy_s, snprintf_s),
# which no libc this builds on has: off, as in Filo's own gate.
TIDY_CHECKS = bugprone-*,cert-*,clang-analyzer-*,readability-*,-readability-magic-numbers,-readability-function-cognitive-complexity,-readability-identifier-length,-readability-braces-around-statements,-bugprone-easily-swappable-parameters,-cert-err33-c,-readability-else-after-return,-readability-avoid-nested-conditional-operator,-readability-math-missing-parentheses,-cert-dcl03-c,-readability-uppercase-literal-suffix,-clang-analyzer-optin.performance.Padding,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling

tidy:
	$(CLANG_TIDY) --quiet --warnings-as-errors='*' --checks='$(TIDY_CHECKS)' \
		src/edt.c src/main.c test/test_edt.c -- -std=c11 -D_DEFAULT_SOURCE $(CFG) $(INC)

check:
	cppcheck --enable=warning,style,performance,portability --inline-suppr \
		--suppress=missingIncludeSystem --error-exitcode=1 $(CFG) $(INC) \
		src/edt.c src/main.c test/test_edt.c

# The binary as shipped, on a terminal: it starts, draws, and quits.
smoke: edt
	sh $(FILO_TERM)/tools/smoke.sh ./edt '\021'

qa: edt fmt-check test smoke tidy check

install: edt
	mkdir -p $(PREFIX)/bin
	cp edt $(PREFIX)/bin/edt

# What release.sh publishes (VERSION is its tag): edt for macOS and for Linux.
DIST_DIR ?= dist
dist: build/fbb.c
	sh $(FILO_TERM)/tools/dist.sh $(DIST_DIR) edt -O2 $(FLAGS) -DFILO_VM_ONLY \
		$(CORE) build/fbb.c src/main.c $(FILO_TERM)/src/tty.c

clean:
	rm -rf build dist edt edt.fbb
