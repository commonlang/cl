# Common (cl) - build, test, install.
#
#   make            build ./cl  (SQLite, Postgres and libcurl are added automatically when installed)
#   make test       run the test suite
#   make sanitize   build with AddressSanitizer + UBSan and run the tests
#   make install    copy cl to /usr/local/bin (PREFIX to change)
#   make downloads  rebuild the zip of the quote example (needs the demo site)
#   make agents     regenerate skills/common/SKILL.md, the skill for AI agents
#   make bench      a rough speed check
#   make release VERSION=X.Y.Z   prepare a release (version, changelog, checks)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PREFIX  ?= /usr/local

# The core is the language; each library is a module behind src/cl_module.h,
# added to the build only when the library is installed.
SRC     := src/cl.c

# Optional Postgres: on if pg_config is installed (Homebrew: brew install libpq; Debian: apt install libpq-dev).
PG_CONFIG := $(shell command -v pg_config 2>/dev/null || ls /opt/homebrew/opt/libpq/bin/pg_config 2>/dev/null)
ifneq ($(PG_CONFIG),)
  PG_INC := $(shell $(PG_CONFIG) --includedir)
  PG_LIB := $(shell $(PG_CONFIG) --libdir)
  PG_OK := $(shell echo 'int main(void){return 0;}' | $(CC) -x c -include libpq-fe.h -I$(PG_INC) - -o /dev/null -L$(PG_LIB) -lpq 2>/dev/null && echo yes)
endif
ifeq ($(PG_OK),yes)
  CFLAGS += -DCL_POSTGRES -I$(PG_INC)
  SRC    += src/mod_postgres.c
  LDLIBS += -L$(PG_LIB) -lpq
endif

# Optional SQLite: on if the compiler can see sqlite3.h and link -lsqlite3.
# (no '#' in this probe: GNU make 3.81 and 4.x treat it differently inside $(shell))
SQLITE_OK := $(shell echo 'int main(void){return 0;}' | $(CC) -x c -include sqlite3.h - -o /dev/null -lsqlite3 2>/dev/null && echo yes)
ifeq ($(SQLITE_OK),yes)
  CFLAGS += -DCL_SQLITE
  SRC    += src/mod_sqlite.c
  LDLIBS += -lsqlite3
endif

# Optional libcurl, for fetch: on if the compiler can see curl/curl.h and link -lcurl
# (Debian: apt install libcurl4-openssl-dev; macOS has it).
CURL_OK := $(shell echo 'int main(void){return 0;}' | $(CC) -x c -include curl/curl.h - -o /dev/null -lcurl 2>/dev/null && echo yes)
ifeq ($(CURL_OK),yes)
  CFLAGS += -DCL_CURL
  SRC    += src/mod_curl.c
  LDLIBS += -lcurl
endif

cl: $(SRC) src/cl_module.h
	$(CC) $(CFLAGS) -o cl $(SRC) $(LDLIBS)
	@echo "built cl (SQLite: $(if $(SQLITE_OK),yes,no), Postgres: $(if $(PG_OK),yes,no), fetch: $(if $(CURL_OK),yes,no))"

test: cl
	sh tests/run.sh

sanitize: $(SRC) src/cl_module.h
	$(CC) -g -O1 -Wall -Wextra -std=c11 $(if $(SQLITE_OK),-DCL_SQLITE) $(if $(PG_OK),-DCL_POSTGRES -I$(PG_INC)) $(if $(CURL_OK),-DCL_CURL) -fsanitize=address,undefined -fno-omit-frame-pointer -o cl $(SRC) $(LDLIBS)
	sh tests/run.sh
	$(MAKE) clean

# Rough speed check: a loop of a million turns, and text with holes built 200,000 times.
bench: cl
	@printf '<cl>\nset n to 0\nset total to 0\nrepeat 1000000 times\n  set n to n + 1\n  set total to total + n * 2\nend\nshow total\n</cl>\n' > .bench-loop.html
	@printf '<cl>\nset n to 0\nrepeat 200000 times\n  set n to n + 1\n  set line to "<li>Item [n] costs [n * 2] dollars</li>"\nend\nshow line\n</cl>\n' > .bench-holes.html
	@for f in loop holes; do /usr/bin/time -p ./cl .bench-$$f.html 2>&1 >/dev/null | awk -v f=$$f '/real/{print "  " f ": " $$2 " s"}'; done
	@rm -f .bench-loop.html .bench-holes.html

# Prepare a release: make release VERSION=0.5.0 (see scripts/release.sh). Never tags or pushes.
release:
	@sh scripts/release.sh "$(VERSION)"

# The skill for AI agents: the hand-written guide, then every word from the interpreter.
# Run after changing skills/SKILL.head.md or any word. make test fails when it is stale.
agents: cl
	{ cat skills/SKILL.head.md; ./cl words --markdown; } > skills/common/SKILL.md

# The downloadable, standalone copy of the quote example, offered on /quote.
# Rebuild after changing examples/downloads/house-quote.html.
downloads:
	@[ -d examples/site ] || { echo "make downloads needs the demo site in examples/site"; exit 1; }
	rm -rf build/house-quote && mkdir -p build/house-quote examples/site/downloads
	cp examples/downloads/house-quote.html build/house-quote/index.html
	cp examples/downloads/house-quote-README.txt build/house-quote/README.txt
	rm -f examples/site/downloads/house-quote.zip
	cd build && zip -qrX ../examples/site/downloads/house-quote.zip house-quote
	rm -rf build

install: cl
	install -d $(PREFIX)/bin
	install -m 755 cl $(PREFIX)/bin/cl

clean:
	rm -rf cl cl.dSYM

.PHONY: test sanitize install clean downloads agents bench release
