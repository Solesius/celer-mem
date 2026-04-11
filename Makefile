CXX       ?= g++
CXXFLAGS  := -std=c++23 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS  := -I include

# Auto-detect RocksDB. Override with CELER_NO_ROCKSDB=1 to force off.
ifndef CELER_NO_ROCKSDB
  HAS_ROCKSDB := $(shell printf '\043include <rocksdb/db.h>\n' | $(CXX) $(CPPFLAGS) -x c++ -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
else
  HAS_ROCKSDB := 0
endif

# Auto-detect SQLite3. Override with CELER_NO_SQLITE=1 to force off.
ifndef CELER_NO_SQLITE
  HAS_SQLITE := $(shell printf '\043include <sqlite3.h>\n' | $(CXX) $(CPPFLAGS) -x c++ -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
else
  HAS_SQLITE := 0
endif

LDFLAGS := -lpthread

ifeq ($(HAS_ROCKSDB),1)
  LDFLAGS += -lrocksdb
else
  CPPFLAGS += -DCELER_FORCE_NO_ROCKSDB
endif

ifeq ($(HAS_SQLITE),1)
  LDFLAGS += -lsqlite3
endif

PREFIX    ?= /usr/local
SRCDIR    := src
BUILDDIR  := build
TESTDIR   := tests
EXDIR     := examples

# ── Sources ──
SRCS := $(shell find $(SRCDIR) -name '*.cpp')
OBJS := $(SRCS:$(SRCDIR)/%.cpp=$(BUILDDIR)/%.o)

# ── Test binaries ──
TEST_BIN      := $(BUILDDIR)/celer_tests
INTEG_BIN     := $(BUILDDIR)/celer_integration
SQLITE_BIN    := $(BUILDDIR)/celer_sqlite_tests

# ── Example binaries ──
EX_SRCS := $(wildcard $(EXDIR)/*.cpp)
EX_BINS := $(EX_SRCS:$(EXDIR)/%.cpp=$(BUILDDIR)/examples/%)

# ── Library ──
LIB := $(BUILDDIR)/libceler.a

.PHONY: all clean test integration test-sqlite examples dirs install uninstall check-headers

all: dirs $(LIB)
	@echo "✓ libceler.a built successfully"

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# ── Unit tests ──
test: dirs $(LIB) $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): $(TESTDIR)/main.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LIB) $(LDFLAGS) -o $@

# ── Integration tests ──
integration: dirs $(LIB) $(INTEG_BIN)
	$(INTEG_BIN)

$(INTEG_BIN): $(TESTDIR)/integration.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LIB) $(LDFLAGS) -o $@

# ── SQLite tests ──
test-sqlite: dirs $(LIB) $(SQLITE_BIN)
	$(SQLITE_BIN)

$(SQLITE_BIN): $(TESTDIR)/test_sqlite.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LIB) $(LDFLAGS) -o $@

# ── Examples ──
examples: dirs $(LIB) $(EX_BINS)
	@echo "✓ All examples built"

$(BUILDDIR)/examples/%: $(EXDIR)/%.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(LIB) $(LDFLAGS) -o $@

# ── Install / Uninstall ──
install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	cp -r include/celer $(DESTDIR)$(PREFIX)/include/
	@echo "✓ Installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f  $(DESTDIR)$(PREFIX)/lib/libceler.a
	rm -rf $(DESTDIR)$(PREFIX)/include/celer
	@echo "✓ Uninstalled from $(DESTDIR)$(PREFIX)"

# ── Helpers ──
dirs:
	@mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

# ── Header check: compile every public header independently ──
HEADERS := $(shell find include -name '*.hpp')
check-headers: dirs
	@echo "Checking headers compile independently..."
	@for h in $(HEADERS); do \
		echo "  $$h"; \
		echo "#include \"$${h#include/}\"" | $(CXX) $(CXXFLAGS) $(CPPFLAGS) -x c++ -fsyntax-only -; \
	done
	@echo "✓ All headers compile independently"
