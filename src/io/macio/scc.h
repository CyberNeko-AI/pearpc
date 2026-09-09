/* Polled Z8530 console at the MacIO legacy SCC register window. */
#ifndef __IO_MACIO_SCC_H__
#define __IO_MACIO_SCC_H__

#include <cstdio>
#include "system/types.h"

class MacIOSCC {
    struct Channel {
        uint8 selected;
        uint8 writeRegs[16];
    } channels[2];
    FILE *output;

public:
    explicit MacIOSCC(FILE *stream = NULL);
    void setOutput(FILE *stream) { output = stream; }
    bool read(uint32 offset, uint32 &data, uint size);
    bool write(uint32 offset, uint32 data, uint size);
};

#endif
