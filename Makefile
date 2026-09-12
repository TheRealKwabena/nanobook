# nanobook — build
#
# Plain make, no dependencies beyond a C++20 compiler. CMakeLists.txt is
# provided too for IDE and CI use; both drive the same sources.

CXX      ?= clang++
STD      := -std=c++20
INCLUDES := -Icpp/include
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Wdouble-promotion

# -march=native matters here: the parser leans on bswap and the ladder on
# clz/ctz, and letting the compiler target this exact core is worth a few
# percent. Drop it for portable binaries.
OPT      := -O3 -march=native -DNDEBUG -fno-omit-frame-pointer
DBG      := -O1 -g -fno-omit-frame-pointer
SAN      := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD    := build
BIN      := $(BUILD)/bin
DATA     := data

TOOLS    := gen_itch replay bench_structures
TOOL_BIN := $(addprefix $(BIN)/,$(TOOLS))

TEST_SRCS := $(wildcard cpp/tests/test_*.cpp)
TEST_BIN  := $(BIN)/run_tests
TEST_SAN  := $(BIN)/run_tests_asan

HEADERS   := $(wildcard cpp/include/nanobook/*.hpp)

.PHONY: all tools tests check bench data clean fmt-check help

all: tools tests

help:
	@echo "make tools     — build gen_itch and replay (optimised)"
	@echo "make tests     — build and run the unit + differential test suite"
	@echo "make check     — tests under AddressSanitizer + UBSan, then end-to-end verify"
	@echo "make data      — generate synthetic feeds into $(DATA)/"
	@echo "make bench     — 10M-message replay benchmark"
	@echo "make clean     — remove $(BUILD)/ and generated feeds"

tools: $(TOOL_BIN)

$(BIN)/%: cpp/tools/%.cpp $(HEADERS) | $(BIN)
	$(CXX) $(STD) $(OPT) $(WARN) $(INCLUDES) $< -o $@

$(BIN):
	@mkdir -p $(BIN)

# ---------------------------------------------------------------------------
# Tests. One translation unit per test file plus a shared runner; the header-only
# library means there is nothing else to link.
# ---------------------------------------------------------------------------
$(TEST_BIN): $(TEST_SRCS) cpp/tests/main.cpp $(HEADERS) | $(BIN)
	$(CXX) $(STD) $(DBG) $(WARN) $(INCLUDES) $(TEST_SRCS) cpp/tests/main.cpp -o $@

$(TEST_SAN): $(TEST_SRCS) cpp/tests/main.cpp $(HEADERS) | $(BIN)
	$(CXX) $(STD) $(SAN) $(WARN) $(INCLUDES) $(TEST_SRCS) cpp/tests/main.cpp -o $@

tests: $(TEST_BIN)
	@$(TEST_BIN)

# The full gate: sanitised unit tests, then a real feed reconstructed and
# compared against independently generated ground truth, message by message.
check: $(TEST_SAN) tools
	@mkdir -p $(DATA)
	@echo "=== unit + property tests under ASan/UBSan ==="
	@$(TEST_SAN)
	@echo
	@echo "=== end-to-end: generate 1M messages across 3 symbols, verify every event ==="
	@$(BIN)/gen_itch --out $(DATA)/check.itch --truth $(DATA)/check_truth.csv \
	    --messages 1000000 --symbols 3 --seed 20240912 >/dev/null
	@$(BIN)/replay --feed $(DATA)/check.itch --symbol NBSYN \
	    --truth $(DATA)/check_truth.csv --quiet --depth 0 \
	    | tail -n 8
	@echo
	@echo "=== off-by-one guard: same feed with a 2-symbol locate layout ==="
	@$(BIN)/gen_itch --out $(DATA)/check2.itch --truth $(DATA)/check2_truth.csv \
	    --messages 200000 --symbols 1 --seed 99 >/dev/null
	@$(BIN)/replay --feed $(DATA)/check2.itch --symbol NBSYN \
	    --truth $(DATA)/check2_truth.csv --quiet --depth 0 | tail -n 3

data: tools
	@mkdir -p $(DATA)
	$(BIN)/gen_itch --out $(DATA)/tiny.itch  --truth $(DATA)/tiny_truth.csv  --messages 20000    --seed 7
	$(BIN)/gen_itch --out $(DATA)/small.itch --truth $(DATA)/small_truth.csv --messages 1000000  --seed 11
	$(BIN)/gen_itch --out $(DATA)/bench.itch --messages 10000000 --seed 1

bench: tools
	@mkdir -p $(DATA)
	@echo "=== data structures vs the standard library ==="
	@$(BIN)/bench_structures --ops 20000000 --live 200000 --reps 3
	@echo
	@echo "=== end-to-end feed replay ==="
	@test -f $(DATA)/bench.itch || $(BIN)/gen_itch --out $(DATA)/bench.itch --messages 10000000 --seed 1
	@echo "warming page cache..."
	@$(BIN)/replay --feed $(DATA)/bench.itch --symbol NBSYN --quiet --depth 0 >/dev/null
	@for i in 1 2 3; do \
	    $(BIN)/replay --feed $(DATA)/bench.itch --symbol NBSYN --quiet --depth 0 \
	      | sed -n '/^throughput/,/feed rate/p'; \
	done

clean:
	rm -rf $(BUILD) $(DATA)/*.itch $(DATA)/*_truth.csv
