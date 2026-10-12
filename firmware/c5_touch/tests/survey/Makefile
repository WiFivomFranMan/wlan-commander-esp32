# SPDX-License-Identifier: MIT
#
# Host build and test of components/wlc_survey.
#   make -C tests/survey check     unit tests, fuzz, golden comparison against tshark, and the
#                                  GCC build below when Homebrew GCC is installed
#   make -C tests/survey unit|fuzz|golden|gcc
#
# `gcc` exists because the firmware is built by riscv32 GCC with -Werror, and GCC's flow-based
# warnings (-Wmaybe-uninitialized above all) depend on the optimisation level and never fire
# under clang. It compiles the library at every level ESP-IDF can select, then builds and runs
# the unit tests and a short fuzz at -O2. Homebrew GCC has no sanitizers on this platform.

LIB      := ../../components/wlc_survey
CXX      ?= clang++
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Werror -Wshadow $(SAN) -I$(LIB)/include -I.
# The library itself must build without exceptions or RTTI, as it will on the device.
LIBFLAGS := $(CXXFLAGS) -fno-exceptions -fno-rtti
BUILD    := build
FUZZ_ITERS ?= 200000
PYTHON   ?= python3

GXX      ?= $(lastword $(sort $(wildcard /opt/homebrew/bin/g++-[0-9]*)))
# Homebrew GCC trails the SDK: point it at the current one and a deployment target its
# assembler driver accepts.
GXXFLAGS := -std=c++17 -Wall -Wextra -Werror -Wshadow -Wmaybe-uninitialized \
            -isysroot $(shell xcrun --show-sdk-path 2>/dev/null) -mmacosx-version-min=15.0 \
            -I$(LIB)/include -I.
GCC_DIR  := $(BUILD)/gcc

LIB_SRCS := $(wildcard $(LIB)/src/*.cpp)
LIB_OBJS := $(patsubst $(LIB)/src/%.cpp,$(BUILD)/lib/%.o,$(LIB_SRCS))
LIB_HDRS := $(wildcard $(LIB)/include/wlc_survey/*.h) $(LIB)/src/internal.h
UNIT_SRCS := test_main.cpp test_ap.cpp test_clients.cpp test_anqp.cpp test_gas.cpp test_api.cpp test_pcap.cpp
UNIT_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(UNIT_SRCS)) $(BUILD)/pcap_reader.o

.PHONY: check unit fuzz golden gcc clean
check: unit fuzz golden $(if $(GXX),gcc)

$(BUILD)/lib/%.o: $(LIB)/src/%.cpp $(LIB_HDRS) | $(BUILD)/lib
	$(CXX) $(LIBFLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp $(LIB_HDRS) harness.h pcap_reader.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD) $(BUILD)/lib:
	mkdir -p $@

$(BUILD)/unit_tests: $(UNIT_OBJS) $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/fuzz: $(BUILD)/fuzz.o $(BUILD)/pcap_reader.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/survey_dump: $(BUILD)/survey_dump.o $(BUILD)/pcap_reader.o $(LIB_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

unit: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

fuzz: $(BUILD)/fuzz
	./$(BUILD)/fuzz $(FUZZ_ITERS) fixtures/*.pcap*

golden: $(BUILD)/survey_dump
	$(PYTHON) golden.py --dump ./$(BUILD)/survey_dump fixtures/*.pcap*

gcc: | $(BUILD)
	@test -n "$(GXX)" || { echo "no Homebrew g++ found"; exit 1; }
	mkdir -p $(GCC_DIR)
	for opt in -Og -Os -O2 -O3; do for f in $(LIB_SRCS); do \
	  $(GXX) $(GXXFLAGS) -fno-exceptions -fno-rtti $$opt -c $$f -o /dev/null || exit 1; done; done
	$(GXX) $(GXXFLAGS) -O2 -fno-exceptions -fno-rtti -c $(LIB)/src/survey.cpp -o $(GCC_DIR)/survey.o
	$(GXX) $(GXXFLAGS) -O2 -fno-exceptions -fno-rtti -c $(LIB)/src/anqp.cpp -o $(GCC_DIR)/anqp.o
	$(GXX) $(GXXFLAGS) -O2 -fno-exceptions -fno-rtti -c $(LIB)/src/format.cpp -o $(GCC_DIR)/format.o
	$(GXX) $(GXXFLAGS) -O2 $(UNIT_SRCS) pcap_reader.cpp $(GCC_DIR)/survey.o $(GCC_DIR)/anqp.o $(GCC_DIR)/format.o -o $(GCC_DIR)/unit_tests
	$(GXX) $(GXXFLAGS) -O2 fuzz.cpp pcap_reader.cpp $(GCC_DIR)/survey.o $(GCC_DIR)/anqp.o $(GCC_DIR)/format.o -o $(GCC_DIR)/fuzz
	./$(GCC_DIR)/unit_tests
	./$(GCC_DIR)/fuzz 50000 fixtures/*.pcap*

clean:
	rm -rf $(BUILD)
