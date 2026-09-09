/*
 * Execute the real stwcx. code generator at every fragment boundary.
 * macOS AArch64 host test; build with run_aarch64_codegen_tests.sh.
 * Only the MMU calls and diagnostic sink are replaced by test doubles.
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>

#include "cpu/cpu_jitc_aarch64/jitc.h"
#include "cpu/cpu_jitc_aarch64/ppc_mmu.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error This native execution test requires macOS AArch64.
#endif

static PPC_CPU_State cpu;
extern "C" {
PPC_CPU_State *gCPU = &cpu;
}

void ppc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void jitcDebugLogEmit(JITC &, const byte *, int) {}

static uint32 memoryWord;
static uint32 reads, writes;
static constexpr uint32 address = 0x1234;

static uint32 readWord(uint32 ea)
{
    if (ea != address) {
        ppc_fatal("read EA: %08x\n", ea);
    }
    reads++;
    return memoryWord;
}

static void writeWord(uint32 ea, uint32 value)
{
    if (ea != address) {
        ppc_fatal("write EA: %08x\n", ea);
    }
    writes++;
    memoryWord = value;
}

// stwcx. calls MMU helpers and overwrites LR. Use X19 for the test return
// address, and preserve the host ABI's callee-saved registers in this wrapper.
extern "C" void runStwcx(PPC_CPU_State *state, NativeAddress entry);
__asm__(
    ".text\n"
    ".p2align 2\n"
    "_runStwcx:\n"
    "stp x19, x20, [sp, #-32]!\n"
    "str x30, [sp, #16]\n"
    "mov x20, x0\n"
    "adr x19, 1f\n"
    "br x1\n"
    "1:\n"
    "ldr x30, [sp, #16]\n"
    "ldp x19, x20, [sp], #32\n"
    "ret\n");

int main()
{
    constexpr size_t cacheSize = 4 * 1024 * 1024;
    byte *cache = static_cast<byte *>(mmap(nullptr, cacheSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
    if (cache == MAP_FAILED) {
        perror("mmap MAP_JIT");
        return 1;
    }

    unsigned checks = 0;
    for (int backwards = 0; backwards < 2; backwards++) {
        for (int indexed = 0; indexed < 2; indexed++) {
            for (uint left = 4; left <= FRAGMENT_SIZE; left += 4) {
                pthread_jit_write_protect_np(0);
                JITC jitc = {};
                ClientPage page = {};
                // Two fragments separated by 2 MiB exercise both directions
                // beyond the range of TBZ and conditional branches.
                TranslationCacheFragment first = {cache + backwards * 2 * 1024 * 1024, nullptr};
                TranslationCacheFragment next = {cache + (1 - backwards) * 2 * 1024 * 1024, nullptr};
                jitc.translationCache = cache;
                jitc.currentPage = &page;
                jitc.freeFragmentsList = &next;
                jitc.nativeFlags = PPC_NO_CRx;
                jitc.pc = 0x688;
                const int rA = indexed ? 4 : 0;
                jitc.current_opc = (31u << 26) | (3 << 21) | (rA << 16) | (5 << 11) | (150 << 1) | 1;
                page.tcf_current = &first;
                page.bytesLeft = left;
                page.tcp = first.base + FRAGMENT_SIZE - left;
                NativeAddress entry = page.tcp;
                cpu.stubs[PPC_STUB_READ_WORD] = reinterpret_cast<NativeAddress>(readWord);
                cpu.stubs[PPC_STUB_WRITE_WORD] = reinterpret_cast<NativeAddress>(writeWord);
                ppc_opc_gen_stwcx_(jitc);
                jitc.asmBR(X19);
                __builtin___clear_cache(reinterpret_cast<char *>(cache), reinterpret_cast<char *>(cache + cacheSize));
                pthread_jit_write_protect_np(1);

                for (int so = 0; so < 2; so++) {
                    // Absent reservation, matching value, and stale value.
                    for (int reservation = 0; reservation < 3; reservation++) {
                        cpu.gpr[3] = 0xcafebabe;
                        cpu.gpr[4] = 0x1000;
                        cpu.gpr[5] = indexed ? address - cpu.gpr[4] : address;
                        cpu.cr = 0xfabcde12;
                        cpu.xer = (so ? XER_SO : 0) | 0x6000007f;
                        cpu.have_reservation = reservation != 0;
                        cpu.reserve = reservation == 2 ? 0xbad : 0x11223344;
                        memoryWord = 0x11223344;
                        reads = writes = 0;
                        runStwcx(&cpu, entry);

                        uint32 expectedCR = 0x0abcde12;
                        if (reservation == 1) {
                            expectedCR |= CR_CR0_EQ;
                        }
                        // Preserve the backend's existing no-reservation behavior.
                        if (reservation != 0 && so) {
                            expectedCR |= CR_CR0_SO;
                        }
                        uint32 expectedWord = reservation == 1 ? 0xcafebabe : 0x11223344;
                        if (cpu.cr != expectedCR || memoryWord != expectedWord || cpu.have_reservation ||
                            reads != uint32(reservation != 0) || writes != uint32(reservation == 1) ||
                            cpu.xer != ((so ? XER_SO : 0) | 0x6000007f)) {
                            ppc_fatal("FAIL backwards=%d indexed=%d left=%u so=%d reservation=%d "
                                      "CR=%08x expected=%08x word=%08x reads=%u writes=%u\n",
                                      backwards, indexed, left, so, reservation, cpu.cr, expectedCR,
                                      memoryWord, reads, writes);
                        }
                        checks++;
                    }
                }
            }
        }
    }
    munmap(cache, cacheSize);
    printf("PASS: %u stwcx. fragment-boundary executions\n", checks);
    return 0;
}
