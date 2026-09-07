# Standalone test harness for the AArch64 JIT
# Build from project root: make -C test
#
# This links against the CPU library and minimal stubs,
# bypassing the full PearPC init (no IO, no PROM, no display).

SRCDIR = ../src
CPUDIR = $(SRCDIR)/cpu/cpu_jitc_aarch64
TOOLSDIR = $(SRCDIR)/tools

CXX = g++
CXXFLAGS = -std=c++11 -g -O0 -I$(SRCDIR) -DHAVE_CONFIG_H -I.. -Wall

# CPU library (built by main build)
CPULIB = $(CPUDIR)/libcpu.a
TOOLSLIB = $(TOOLSDIR)/libtools.a
DEBUGLIB = $(SRCDIR)/debug/libdebug.a

# We need a few stubs for symbols that the CPU lib references
# but that aren't needed for the test harness

all: test_semantics test_ide_io

jitc_test: jitc_test.o stubs.o $(CPULIB)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(TOOLSLIB) $(DEBUGLIB) -lpthread

jitc_test.o: jitc_test.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

stubs.o: stubs.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Standalone semantics/liveness test — header-only, no CPU lib needed
test_semantics: test_semantics.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test_semantics.o: test_semantics.cc
	$(CXX) $(CXXFLAGS) -include ../config.h -c -o $@ $<

test_ide_io: test_ide_io.o $(SRCDIR)/io/ide/ata.o $(SRCDIR)/io/ide/cd.o $(SRCDIR)/io/ide/idedevice.o $(SRCDIR)/system/osapi/posix/sysfile.o $(SRCDIR)/system/osapi/posix/systhread.o $(SRCDIR)/system/sys.o $(SRCDIR)/system/file.o $(TOOLSLIB) $(SRCDIR)/system/arch/generic/libsarch.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread

test_ide_io.o: test_ide_io.cc
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f jitc_test test_semantics test_ide_io *.o

.PHONY: all clean
