// Regression for Darwin's polled console during display mode changes.
// c++ -std=c++11 -DHAVE_CONFIG_H -I. -Isrc test/test_macio_scc.cc src/io/macio/scc.cc -o /tmp/test_macio_scc
#include <cassert>
#include <cstring>
#include "io/macio/scc.h"

int main()
{
    FILE *log = tmpfile();
    assert(log);
    MacIOSCC scc(log);
    uint32 data = 0;
    const uint32 controls[] = {0, 2};
    for (uint32 control : controls) {
        // initialize_serial: channel reset, clock divisor, transmit enable.
        assert(scc.write(control, 9, 1));
        assert(scc.write(control, control ? 0x80 : 0x40, 1));
        assert(scc.write(control, 12, 1));
        assert(scc.write(control, 0x0a, 1));
        assert(scc.write(control, 13, 1));
        assert(scc.write(control, 0, 1));
        assert(scc.write(control, 5, 1));
        assert(scc.write(control, 0xea, 1));

        assert(scc.write(control, 12, 1));
        assert(scc.read(control, data, 1) && data == 0x0a);
        // Register selection is consumed by the read; next access is RR0.
        assert(scc.read(control, data, 1) && (data & 4) && !(data & 1));
        assert(scc.write(control, 1, 1));
        assert(scc.read(control, data, 1) && (data & 1));
    }

    // Channel register pointers are independent, including POINT HIGH.
    assert(scc.write(0, 12, 1));
    assert(scc.write(0, 0x55, 1));
    assert(scc.write(0, 12, 1));
    assert(scc.read(2, data, 1) && data == 0x2c);
    assert(scc.read(0, data, 1) && data == 0x55);

    // scc_putc's poll-write-poll sequence must complete without receive input.
    const char message[] = "PearPCVideo: vram\n";
    for (const char *c = message; *c; ++c) {
        assert(scc.write(2, 0, 1));
        assert(scc.read(2, data, 1) && (data & 4));
        assert(scc.write(6, *c, 1));
        assert(scc.read(2, data, 1) && (data & 4));
        assert(scc.write(2, 0x38, 1)); // Reset highest interrupt under service.
    }
    rewind(log);
    char captured[sizeof message] = {};
    assert(fread(captured, 1, sizeof message - 1, log) == sizeof message - 1);
    assert(strcmp(captured, message) == 0);

    assert(scc.write(2, 9, 1));
    assert(scc.write(2, 0xc0, 1)); // Full reset also clears B's divisor.
    assert(scc.write(0, 12, 1));
    assert(scc.read(0, data, 1) && data == 0);
    assert(!scc.read(1, data, 1));
    assert(!scc.read(8, data, 1));
    assert(!scc.write(2, 0, 4));
    fclose(log);
    puts("MacIO SCC console tests passed");
}
