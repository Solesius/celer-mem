CXX       ?= g++
CXXFLAGS  := -std=c++23 -Wall -Wextra -Wpedantic -Werror -O2
CPPFLAGS  := -I include

SRCDIR    := src
BUILDDIR  := build
TESTDIR   := tests

# ── Sources ──
SRCS := $(shell find $(SRCDIR) -name '*.cpp')
OBJS := $(SRCS:$(SRCDIR)/%.cpp=$(BUILDDIR)/%.o)

# ── Test sources ──
TEST_SRCS := $(shell find $(TESTDIR) -name '*.cpp' 2>/dev/null)
TEST_OBJS := $(TEST_SRCS:$(TESTDIR)/%.cpp=$(BUILDDIR)/tests/%.o)
TEST_BIN  := $(BUILDDIR)/celer_tests

# ── Library ──
LIB := $(BUILDDIR)/libceler.a

.PHONY: all clean test dirs

all: dirs $(LIB)
	@echo "✓ libceler.a built successfully"

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# ── Tests ──
test: dirs $(LIB) $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILDDIR)/tests/%.o: $(TESTDIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# ── Helpers ──
dirs:
	@mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

# ── Header check: compile every public header independently ──
HEADERS := $(shell find include -name '*.hpp')
.PHONY: check-headers
check-headers: dirs
	@echo "Checking headers compile independently..."
	@for h in $(HEADERS); do \
		echo "  $$h"; \
		echo "#include \"$${h#include/}\"" | $(CXX) $(CXXFLAGS) $(CPPFLAGS) -x c++ -fsyntax-only -; \
	done
	@echo "✓ All headers compile independently"
