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

# Auto-detect QPDF. Override with CELER_NO_QPDF=1 to force off.
ifndef CELER_NO_QPDF
  HAS_QPDF := $(shell printf '\043include <qpdf/QPDF.hh>\n' | $(CXX) $(CPPFLAGS) -x c++ -fsyntax-only - 2>/dev/null && echo 1 || echo 0)
else
  HAS_QPDF := 0
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

ifeq ($(HAS_QPDF),1)
  LDFLAGS += -lqpdf
else
  CPPFLAGS += -DCELER_FORCE_NO_QPDF
endif

PREFIX    ?= /usr/local
SRCDIR    := src
BUILDDIR  := build
TESTDIR   := tests
EXDIR     := examples
CMAKE_BUILDDIR := $(BUILDDIR)/cmake
CMAKE_FLAGS    := -DCELER_BUILD_TESTS=ON -DCELER_BUILD_EXAMPLES=OFF -DCELER_BUILD_S3=OFF

# ── Sources ──
SRCS := $(shell find $(SRCDIR) -name '*.cpp')
OBJS := $(SRCS:$(SRCDIR)/%.cpp=$(BUILDDIR)/%.o)

# ── Test binaries ──
TEST_BIN      := $(BUILDDIR)/celer_tests
INTEG_BIN     := $(BUILDDIR)/celer_integration
SQLITE_BIN    := $(BUILDDIR)/celer_sqlite_tests
QPDF_BIN      := $(BUILDDIR)/celer_qpdf_tests
ASYNC_BIN     := $(BUILDDIR)/celer_async_tests

# ── Example binaries ──
EX_SRCS := $(wildcard $(EXDIR)/*.cpp)
EX_BINS := $(EX_SRCS:$(EXDIR)/%.cpp=$(BUILDDIR)/examples/%)

# ── Library ──
LIB := $(BUILDDIR)/libceler.a

.PHONY: all clean cmake-configure test test-all integration test-sqlite test-qpdf test-async coverage examples dirs install uninstall check-headers

all: dirs $(LIB)
	@echo "✓ libceler.a built successfully"

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# ── CMake/CTest-backed test targets ──
cmake-configure:
	cmake -S . -B $(CMAKE_BUILDDIR) $(CMAKE_FLAGS)

# ── Unit tests ──
test: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) --target celer_tests -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) -L unit --output-on-failure

test-all: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) --output-on-failure

# ── Integration tests ──
integration: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) --target celer_integration -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) -L integration --output-on-failure

# ── SQLite tests ──
test-sqlite: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) --target celer_sqlite_tests -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) -L sqlite --output-on-failure

# ── QPDF tests ──
test-qpdf: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) --target celer_qpdf_tests -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) -L qpdf --output-on-failure

# ── Async tests ──
test-async: cmake-configure
	cmake --build $(CMAKE_BUILDDIR) --target celer_async_tests -j$$(nproc)
	ctest --test-dir $(CMAKE_BUILDDIR) -L async --output-on-failure

# ── Coverage ──
coverage:
	cmake -S . -B $(CMAKE_BUILDDIR) $(CMAKE_FLAGS) -DCELER_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(CMAKE_BUILDDIR) --target coverage -j$$(nproc)

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
