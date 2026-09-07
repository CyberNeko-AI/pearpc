/* 
 *	HT Editor
 *	sysvaccel.cc - generic implementation
 *
 *	Copyright (C) 2004 Stefan Weyergraf
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <cstring>
#include "system/sysvaccel.h"

#include "tools/snprintf.h"

static inline void convertBaseColor(uint &b, uint fromBits, uint toBits)
{
    if (toBits > fromBits) {
        b <<= toBits - fromBits;
    } else {
        b >>= fromBits - toBits;
    }
}

#if defined(__aarch64__)
#include <arm_neon.h>

static void aarch64_neon_convert_4be_to_4le(const byte *src, byte *dest, size_t num_pixels)
{
    size_t i = 0;
    // Process 16 pixels (64 bytes) per iteration
    for (; i + 16 <= num_pixels; i += 16) {
        uint8x16_t v0 = vld1q_u8(src + i * 4);
        uint8x16_t v1 = vld1q_u8(src + (i + 4) * 4);
        uint8x16_t v2 = vld1q_u8(src + (i + 8) * 4);
        uint8x16_t v3 = vld1q_u8(src + (i + 12) * 4);

        v0 = vrev32q_u8(v0);
        v1 = vrev32q_u8(v1);
        v2 = vrev32q_u8(v2);
        v3 = vrev32q_u8(v3);

        vst1q_u8(dest + i * 4, v0);
        vst1q_u8(dest + (i + 4) * 4, v1);
        vst1q_u8(dest + (i + 8) * 4, v2);
        vst1q_u8(dest + (i + 12) * 4, v3);
    }
    // Tail: 4 pixels at a time
    for (; i + 4 <= num_pixels; i += 4) {
        uint8x16_t v = vld1q_u8(src + i * 4);
        v = vrev32q_u8(v);
        vst1q_u8(dest + i * 4, v);
    }
    // Remainder: 1 pixel at a time
    for (; i < num_pixels; i++) {
        uint32 p;
        memcpy(&p, src + i * 4, 4);
        p = __builtin_bswap32(p);
        memcpy(dest + i * 4, &p, 4);
    }
}

static void aarch64_neon_convert_2be_to_2le(const byte *src, byte *dest, size_t num_pixels)
{
    size_t i = 0;
    // Process 16 pixels (32 bytes) per iteration
    for (; i + 16 <= num_pixels; i += 16) {
        uint8x16_t v0 = vld1q_u8(src + i * 2);
        uint8x16_t v1 = vld1q_u8(src + (i + 8) * 2);

        v0 = vrev16q_u8(v0);
        v1 = vrev16q_u8(v1);

        vst1q_u8(dest + i * 2, v0);
        vst1q_u8(dest + (i + 8) * 2, v1);
    }
    for (; i + 8 <= num_pixels; i += 8) {
        uint8x16_t v = vld1q_u8(src + i * 2);
        v = vrev16q_u8(v);
        vst1q_u8(dest + i * 2, v);
    }
    for (; i < num_pixels; i++) {
        uint16 p;
        memcpy(&p, src + i * 2, 2);
        p = __builtin_bswap16(p);
        memcpy(dest + i * 2, &p, 2);
    }
}
#endif

void sys_convert_display(const DisplayCharacteristics &aSrcChar, const DisplayCharacteristics &aDestChar,
                         const void *aSrcBuf, void *aDestBuf, int firstLine, int lastLine)
{
#if defined(__aarch64__)
    // Fast path: 32bpp identical channels (typical Mac OS X desktop: ARGB BE -> XRGB8888 LE)
    if (aSrcChar.bytesPerPixel == 4 && aDestChar.bytesPerPixel == 4 && aSrcChar.redSize == 8 &&
        aDestChar.redSize == 8 && aSrcChar.greenSize == 8 && aDestChar.greenSize == 8 && aSrcChar.blueSize == 8 &&
        aDestChar.blueSize == 8 && aSrcChar.redShift == aDestChar.redShift &&
        aSrcChar.greenShift == aDestChar.greenShift && aSrcChar.blueShift == aDestChar.blueShift) {
        const byte *src = (const byte *)aSrcBuf + aSrcChar.bytesPerPixel * aSrcChar.width * firstLine;
        byte *dest = (byte *)aDestBuf + aDestChar.bytesPerPixel * aDestChar.width * firstLine;

        if (aSrcChar.scanLineLength == aSrcChar.width * 4 && aDestChar.scanLineLength == aDestChar.width * 4) {
            size_t total_pixels = (size_t)(lastLine - firstLine + 1) * aSrcChar.width;
            aarch64_neon_convert_4be_to_4le(src, dest, total_pixels);
        } else {
            for (int y = firstLine; y <= lastLine; y++) {
                aarch64_neon_convert_4be_to_4le(src, dest, aSrcChar.width);
                src += aSrcChar.scanLineLength;
                dest += aDestChar.scanLineLength;
            }
        }
        return;
    }

    // Fast path: 16bpp identical channels (16-bit BE -> 16-bit LE)
    if (aSrcChar.bytesPerPixel == 2 && aDestChar.bytesPerPixel == 2 && aSrcChar.redSize == aDestChar.redSize &&
        aSrcChar.greenSize == aDestChar.greenSize && aSrcChar.blueSize == aDestChar.blueSize &&
        aSrcChar.redShift == aDestChar.redShift && aSrcChar.greenShift == aDestChar.greenShift &&
        aSrcChar.blueShift == aDestChar.blueShift) {
        const byte *src = (const byte *)aSrcBuf + aSrcChar.bytesPerPixel * aSrcChar.width * firstLine;
        byte *dest = (byte *)aDestBuf + aDestChar.bytesPerPixel * aDestChar.width * firstLine;

        if (aSrcChar.scanLineLength == aSrcChar.width * 2 && aDestChar.scanLineLength == aDestChar.width * 2) {
            size_t total_pixels = (size_t)(lastLine - firstLine + 1) * aSrcChar.width;
            aarch64_neon_convert_2be_to_2le(src, dest, total_pixels);
        } else {
            for (int y = firstLine; y <= lastLine; y++) {
                aarch64_neon_convert_2be_to_2le(src, dest, aSrcChar.width);
                src += aSrcChar.scanLineLength;
                dest += aDestChar.scanLineLength;
            }
        }
        return;
    }
#endif

    byte *src = (byte *)aSrcBuf + aSrcChar.bytesPerPixel * aSrcChar.width * firstLine;
    byte *dest = (byte *)aDestBuf + aDestChar.bytesPerPixel * aDestChar.width * firstLine;
    for (int y = firstLine; y <= lastLine; y++) {
        for (int x = 0; x < aSrcChar.width; x++) {
            uint r, g, b;
            uint p;
            switch (aSrcChar.bytesPerPixel) {
            case 2: p = (src[0] << 8) | src[1]; break;
            case 4: p = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3]; break;
            default:
                ht_printf("internal error in %s:%d\n", __FILE__, __LINE__);
                exit(1);
                break;
            }
            r = (p >> aSrcChar.redShift) & ((1 << aSrcChar.redSize) - 1);
            g = (p >> aSrcChar.greenShift) & ((1 << aSrcChar.greenSize) - 1);
            b = (p >> aSrcChar.blueShift) & ((1 << aSrcChar.blueSize) - 1);
            convertBaseColor(r, aSrcChar.redSize, aDestChar.redSize);
            convertBaseColor(g, aSrcChar.greenSize, aDestChar.greenSize);
            convertBaseColor(b, aSrcChar.blueSize, aDestChar.blueSize);
            p = (r << aDestChar.redShift) | (g << aDestChar.greenShift) | (b << aDestChar.blueShift);
            switch (aDestChar.bytesPerPixel) {
            case 2: *(uint16 *)dest = p; break;
            case 3:
                dest[0] = p;
                dest[1] = p >> 8;
                dest[2] = p >> 16;
                break;
            case 4: *(uint32 *)dest = p; break;
            default: ht_printf("internal error in %s:%d\n", __FILE__, __LINE__); exit(1);
            }
            dest += aDestChar.bytesPerPixel;
            src += aSrcChar.bytesPerPixel;
        }
        dest += aDestChar.scanLineLength - aDestChar.width * aDestChar.bytesPerPixel;
        src += aSrcChar.scanLineLength - aSrcChar.width * aSrcChar.bytesPerPixel;
    }
}
