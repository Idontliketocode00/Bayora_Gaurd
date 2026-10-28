CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread
HDRS     := $(wildcard include/*.hpp)

all: bin/blue bin/gateway bin/red bin/selftest

bin/blue: blue/main.cpp $(HDRS)
	@mkdir -p bin && $(CXX) $(CXXFLAGS) $< -o $@
bin/gateway: gateway/main.cpp $(HDRS)
	@mkdir -p bin && $(CXX) $(CXXFLAGS) $< -o $@
bin/red: red/main.cpp $(HDRS)
	@mkdir -p bin && $(CXX) $(CXXFLAGS) $< -o $@
bin/selftest: tests/selftest.cpp $(HDRS)
	@mkdir -p bin && $(CXX) $(CXXFLAGS) $< -o $@

test: bin/selftest
	./bin/selftest

clean:
	rm -rf bin

.PHONY: all test clean
