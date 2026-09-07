/*
 *	Test and benchmark harness for IDE disk and CD-ROM I/O optimizations
 *	Tests mmap zero-copy, pread/pwrite fallback, batch DMA transfers,
 *	and data integrity.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <unistd.h>

#include "system/types.h"
#include "system/file.h"
#include "cpu/mem.h"
#include "io/ide/idedevice.h"
#include "io/ide/ata.h"
#include "io/ide/cd.h"

static int failures = 0;
static int tests = 0;

#define CHECK(desc, cond)                                                                                      \
    do {                                                                                                       \
        tests++;                                                                                               \
        if (!(cond)) {                                                                                         \
            fprintf(stderr, "FAIL: %s (line %d)\n", desc, __LINE__);                                           \
            failures++;                                                                                        \
        } else {                                                                                               \
            fprintf(stderr, "PASS: %s\n", desc);                                                               \
        }                                                                                                      \
    } while (0)

// Provide minimal dummy symbols if not linking full emulator
uint32 gMemorySize = 16 * 1024 * 1024; // 16 MB fake memory
byte fake_memory[16 * 1024 * 1024];
byte *gMemory = fake_memory;

byte *ppc_dma_get_ptr(uint32 addr, uint32 size)
{
    if (addr > gMemorySize || (addr + size) > gMemorySize) {
        return nullptr;
    }
    return &gMemory[addr];
}

#include <cstdarg>
void ppc_fatal(char const *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

int main()
{
    printf("=== Starting IDE Disk & I/O Optimizations Test Suite ===\n\n");

    // ------------------------------------------------------------------------
    // Test 1: Memory DMA pointer helper (ppc_dma_get_ptr)
    // ------------------------------------------------------------------------
    printf("--- Test 1: ppc_dma_get_ptr bounds checking ---\n");
    byte *ptr = ppc_dma_get_ptr(0, 65536);
    CHECK("ppc_dma_get_ptr at offset 0", ptr == gMemory);

    ptr = ppc_dma_get_ptr(gMemorySize - 4096, 4096);
    CHECK("ppc_dma_get_ptr at end of memory", ptr == gMemory + (gMemorySize - 4096));

    ptr = ppc_dma_get_ptr(gMemorySize - 100, 101);
    CHECK("ppc_dma_get_ptr bounds overflow returns nullptr", ptr == nullptr);

    ptr = ppc_dma_get_ptr(gMemorySize + 1000, 4);
    CHECK("ppc_dma_get_ptr completely out of bounds returns nullptr", ptr == nullptr);

    // ------------------------------------------------------------------------
    // Test 2: ATA Disk Device with mmap & batch read/write
    // ------------------------------------------------------------------------
    printf("\n--- Test 2: ATADeviceFile mmap zero-copy and batch I/O ---\n");
    const char *test_disk_path = "/tmp/pearpc_test_disk.img";
    const uint64 disk_size = 516096ULL * 4; // 4 cylinders = 2,064,384 bytes (~2 MB)

    // Create test image filled with a pseudo-random pattern
    FILE *f = fopen(test_disk_path, "wb");
    CHECK("Create temporary disk image file", f != NULL);
    if (!f) return 1;

    byte pattern[4096];
    for (uint i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (byte)((i * 17 + 31) & 0xff);
    }
    for (uint64 written = 0; written < disk_size; written += sizeof(pattern)) {
        fwrite(pattern, 1, sizeof(pattern), f);
    }
    fclose(f);

    {
        ATADeviceFile disk("test_ata", test_disk_path);
        CHECK("ATADeviceFile opened successfully", disk.getError() == NULL);
        CHECK("Block size is 512", disk.getBlockSize() == 512);
        CHECK("Block count matches", disk.getBlockCount() == disk_size / 512);

        // Test single block read
        byte single_buf[512];
        disk.seek(0);
        int r = disk.readBlock(single_buf);
        CHECK("readBlock succeeded", r == 0);
        CHECK("Single block data matches pattern", memcmp(single_buf, pattern, 512) == 0);

        // Test multi-sector batch read (64 KiB = 128 sectors)
        const int batch_size = 64 * 1024;
        byte batch_read_buf[batch_size];
        disk.seek(2); // offset = 1024
        int bytes_read = disk.read(batch_read_buf, batch_size);
        CHECK("Batch read 64 KiB returned correct size", bytes_read == batch_size);

        // Verify batch data matches expected repeating pattern starting at offset 1024
        bool pattern_match = true;
        for (int i = 0; i < batch_size; i++) {
            byte expected = pattern[(1024 + i) % sizeof(pattern)];
            if (batch_read_buf[i] != expected) {
                fprintf(stderr, "Mismatch at %d: got 0x%02x, expected 0x%02x\n", i, batch_read_buf[i], expected);
                pattern_match = false;
                break;
            }
        }
        CHECK("Batch read 64 KiB data integrity verified", pattern_match);

        // Test multi-sector batch write
        byte write_buf[batch_size];
        for (int i = 0; i < batch_size; i++) {
            write_buf[i] = (byte)(0xAA ^ (i & 0xFF));
        }
        disk.seek(4); // offset = 2048
        int bytes_written = disk.write(write_buf, batch_size);
        CHECK("Batch write 64 KiB returned correct size", bytes_written == batch_size);
        disk.flush();

        // Read back written data and verify
        byte verify_buf[batch_size];
        disk.seek(4);
        disk.read(verify_buf, batch_size);
        CHECK("Read-back written data matches exactly", memcmp(verify_buf, write_buf, batch_size) == 0);
    }

    // ------------------------------------------------------------------------
    // Test 3: Benchmark Legacy Libc 512B vs Phase 5 mmap 64KB Batch
    // ------------------------------------------------------------------------
    fprintf(stderr, "\n--- Test 3: I/O Throughput Benchmark (Legacy libc 512B vs Phase 5 mmap 64KB) ---\n");
    {
        const int iterations = 300;
        const int test_bytes = 64 * 1024; // 64 KiB per iteration -> ~20 MB total
        byte read_buf[test_bytes];

        // 1. Legacy PearPC mechanism: FILE* fseeko + 128x fread(512)
        FILE *raw_file = fopen(test_disk_path, "rb");
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iterations; it++) {
            fseeko(raw_file, 0, SEEK_SET);
            for (int s = 0; s < test_bytes / 512; s++) {
                if (fread(read_buf + s * 512, 1, 512, raw_file) != 512) break;
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_legacy_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        fclose(raw_file);

        // 2. Phase 5 optimized mechanism: mmap zero-copy batch 64KB
        ATADeviceFile disk("bench_ata", test_disk_path);
        // Warmup
        disk.seek(0);
        disk.read(read_buf, test_bytes);

        auto t2 = std::chrono::high_resolution_clock::now();
        for (int it = 0; it < iterations; it++) {
            disk.seek(0);
            disk.read(read_buf, test_bytes);
        }
        auto t3 = std::chrono::high_resolution_clock::now();
        double elapsed_batch_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

        double mb_transferred = (double)(iterations * test_bytes) / (1024.0 * 1024.0);
        fprintf(stderr, "Legacy libc (fseeko+fread 512B): %.3f ms (Throughput: %.1f MB/s)\n",
               elapsed_legacy_ms, mb_transferred / (elapsed_legacy_ms / 1000.0));
        fprintf(stderr, "Phase 5 mmap Batch (64KB direct): %.3f ms (Throughput: %.1f MB/s)\n",
               elapsed_batch_ms, mb_transferred / (elapsed_batch_ms / 1000.0));
        fprintf(stderr, ">>> Speedup: %.2fx faster!\n", elapsed_legacy_ms / elapsed_batch_ms);

        CHECK("Phase 5 mmap batch is faster than legacy libc 512B loop", elapsed_batch_ms < elapsed_legacy_ms);
    }

    // ------------------------------------------------------------------------
    // Test 4: CDROMDeviceFile with mmap & batch reading
    // ------------------------------------------------------------------------
    printf("\n--- Test 4: CDROMDeviceFile mmap zero-copy and batch reading ---\n");
    const char *test_iso_path = "/tmp/pearpc_test_cd.iso";
    const uint64 iso_size = 2048ULL * 1024; // 1024 sectors of 2048B = 2 MB

    f = fopen(test_iso_path, "wb");
    CHECK("Create temporary ISO file", f != NULL);
    if (!f) return 1;

    for (uint64 written = 0; written < iso_size; written += sizeof(pattern)) {
        fwrite(pattern, 1, sizeof(pattern), f);
    }
    fclose(f);

    {
        CDROMDeviceFile cd("test_cd");
        bool ok = cd.changeDataSource(test_iso_path);
        CHECK("changeDataSource succeeded", ok);
        CHECK("CDROM capacity matches", cd.getCapacity() == iso_size / 2048);

        // Test single 2048B sector read
        byte cd_buf[2048];
        cd.setMode(IDE_ATAPI_TRANSFER_DATA, 2048);
        cd.seek(5);
        cd.readBlock(cd_buf);

        // Verify data at sector 5 (offset = 5 * 2048 = 10240)
        bool cd_sector_match = true;
        for (int i = 0; i < 2048; i++) {
            if (cd_buf[i] != pattern[(10240 + i) % sizeof(pattern)]) {
                cd_sector_match = false;
                break;
            }
        }
        CHECK("CD sector read data matches pattern", cd_sector_match);

        // Test batch read (64 KiB = 32 CD sectors)
        byte cd_batch_buf[64 * 1024];
        cd.seek(10);
        int r = cd.read(cd_batch_buf, 64 * 1024);
        CHECK("CD batch read 64 KiB returned correct size", r == 64 * 1024);

        bool cd_batch_match = true;
        for (int i = 0; i < 64 * 1024; i++) {
            if (cd_batch_buf[i] != pattern[(10 * 2048 + i) % sizeof(pattern)]) {
                cd_batch_match = false;
                break;
            }
        }
        CHECK("CD batch read 64 KiB data integrity verified", cd_batch_match);
    }

    // Clean up temporary files
    unlink(test_disk_path);
    unlink(test_iso_path);

    printf("\n=== Summary: %d tests, %d failures ===\n", tests, failures);
    return failures ? 1 : 0;
}
