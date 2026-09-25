# lab3-reduced: MOESI-family coherence simulator.
#
#   make            build build/sim
#   make test       build and run every tests/test_*.cpp
#   make traces     regenerate the archetype traces
#   make format     clang-format the tree
#   make clean
#
# To run the simulator, build first and then use ./sweep:
#
#   make && ./sweep              ideal bus and memory, traffic only
#   make && ./sweep check        the same, with state-machine checking
#
# See ./sweep --help for the bandwidth flags.

CXX      := clang++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude

SRC_DIR  := src
TEST_DIR := tests
OBJ_DIR  := build

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(SRCS))
BIN  := $(OBJ_DIR)/sim

# Everything except main.o, so test binaries can supply their own main().
LIB_OBJS := $(filter-out $(OBJ_DIR)/main.o,$(OBJS))

# Each tests/test_*.cpp builds into its own binary. Adding a test file needs no
# Makefile change.
TEST_SRCS := $(wildcard $(TEST_DIR)/test_*.cpp)
TEST_BINS := $(patsubst $(TEST_DIR)/%.cpp,$(OBJ_DIR)/%,$(TEST_SRCS))

.PHONY: all test traces format clean
.SECONDARY: $(OBJS)

all: $(BIN)

$(BIN): $(OBJS) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

# Tests run from the repo root because fixtures are referenced as
# tests/traces/*.trace. Keeps going after a failure so one broken suite does
# not hide the others, then fails the target.
test: $(TEST_BINS)
	@status=0; \
	for binary in $(TEST_BINS); do \
	  echo "== $$binary"; \
	  ./$$binary || status=1; \
	done; \
	exit $$status

# Regenerates the two archetype traces.
traces:
	@python3 traces/generators/gen_archetypes.py

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%: $(TEST_DIR)/%.cpp $(LIB_OBJS) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -I$(TEST_DIR) $< $(LIB_OBJS) -o $@

$(OBJ_DIR):
	mkdir -p $@

format:
	clang-format -i $(SRC_DIR)/*.cpp include/*.hpp $(TEST_DIR)/*.cpp $(TEST_DIR)/*.hpp

clean:
	rm -rf $(OBJ_DIR)

-include $(wildcard $(OBJ_DIR)/*.d)
