# Dependency-free build: needs only a C++17 compiler and make.
#   make          build ./cultivation (debug console) and ./cx_tests
#   make test     build and run the tests
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Isrc
LIB_SRC  := src/core/save.cpp src/sim/world.cpp src/sim/simulation.cpp
HEADERS  := $(wildcard src/*/*.hpp)

all: cultivation cx_tests

cultivation: src/app/main.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) src/app/main.cpp $(LIB_SRC) -o $@

cx_tests: tests/test_main.cpp $(LIB_SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) tests/test_main.cpp $(LIB_SRC) -o $@

test: cx_tests
	./cx_tests

clean:
	rm -f cultivation cx_tests

.PHONY: all test clean
