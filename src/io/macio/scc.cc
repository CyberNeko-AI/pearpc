#include <cstring>
#include "scc.h"

MacIOSCC::MacIOSCC(FILE *stream) : output(stream)
{
    memset(channels, 0, sizeof channels);
}

bool MacIOSCC::read(uint32 offset, uint32 &data, uint size)
{
    // Legacy mapping: B control, A control, B data, A data, spaced two bytes apart.
    if (size != 1 || offset > 6 || (offset & 1)) {
        return false;
    }
    Channel &channel = channels[(offset >> 1) & 1];
    if (offset & 4) {
        data = 0; // No attached receive source.
        return true;
    }
    uint8 reg = channel.selected;
    channel.selected = 0;
    switch (reg) {
    case 0:
        data = 0x2c; // CTS, DCD and transmitter empty; no receive character pending.
        break;
    case 1:
        data = 1; // All characters sent, no receive errors.
        break;
    case 2:
    case 12:
    case 13:
        data = channel.writeRegs[reg];
        break;
    default:
        data = 0; // No interrupts or FIFO entries pending.
        break;
    }
    return true;
}

bool MacIOSCC::write(uint32 offset, uint32 data, uint size)
{
    if (size != 1 || offset > 6 || (offset & 1)) {
        return false;
    }
    unsigned index = (offset >> 1) & 1;
    Channel &channel = channels[index];
    if (offset & 4) {
        // Transmit synchronously, so RR0/RR1 always report completion.
        if (output) {
            fputc(data & 0xff, output);
            if ((data & 0xff) == '\n') {
                fflush(output);
            }
        }
        return true;
    }
    uint8 reg = channel.selected;
    channel.selected = 0;
    if (reg) {
        channel.writeRegs[reg] = data;
        if (reg == 9) {
            // WR9 reset commands: bit 6 resets B, bit 7 resets A, both reset the chip.
            if (data & 0x40) {
                memset(&channels[0], 0, sizeof channels[0]);
            }
            if (data & 0x80) {
                memset(&channels[1], 0, sizeof channels[1]);
            }
        }
    } else {
        // WR0 selects RR/WR 1-7; its POINT HIGH command selects registers 8-15.
        channel.selected = (data & 7) | (((data & 0x38) == 8) ? 8 : 0);
    }
    return true;
}
