# PearPC on AArch64 (Apple Silicon): 运行效率评估与优化路线图

本文档记录了 PearPC 在 Apple Silicon (macOS arm64) 架构下的性能现状评估、基于底层采样的热点分析根因诊断，以及系统性的分阶段优化演进路线图。

---

## 1. 运行效率现状与基准数据

### 1.1 基准测试环境
- **Host 硬件**: Apple Silicon (arm64, macOS Darwin)
- **模拟器目标**: PowerPC G4 (7400/7410, PVR 0x000c0201)
- **编译配置**: `./configure --enable-ui=sdl --enable-release`
- **基准测试程序**: `test/test_bench.elf` (128 KiB 数据量，包括 xorshift32 PRNG、MD5 完整性哈希、LZSS 字典压缩与解压比对)

### 1.2 性能对比

| 指标 | Host 原生 C (clang -O3) | PearPC AArch64 JIT | 相对差距 |
| :--- | :---: | :---: | :---: |
| **执行耗时 (User Time)** | ~0.07 秒 | ~4.28 秒 | **~60x** |
| **调度次数 (`jitcNewPC`)** | N/A | 240,000,000 次 | 5,600 万次/秒 |
| **正确性校验** | PASS (MD5 matches) | PASS (MD5 matches) | 完全一致 |

对比业界成熟的动态翻译系统（QEMU TCG 约 5~10x、Dolphin 约 2~3x），当前 AArch64 JIT 仍处于“正确性验证完备，但尚未充分榨干 Host 硬件性能”的初始阶段。

---

## 2. 性能瓶颈根因诊断 (Sampling Profile Analysis)

使用 macOS 原生采样器 `/usr/bin/sample` 采集基准测试执行期间的调用栈，得到 CPU 时间分布如下：

```
Total CPU Time
├── 85.5% 分支调度与地址翻译开销 (Dispatch & MMU Overhead)
│   ├── ~50.0%  ppc_effective_to_physical (EA->PA 软件页表遍历与 IBAT 查找)
│   ├── ~32.0%  jitcNewPC (ClientPage 查找、LRU 链表更新、1024-entry 数组索引)
│   └── ~3.5%   ppc_heartbeat_ext_asm (心跳异常检测)
└── ~14.5% JIT 生成的原生指令实际执行 (Native Instruction Execution)
    ├── ~8.0%   ppc_read/write_effective_*_asm (数据访存 TLB 命中及REV字节翻转)
    └── ~6.5%   原生 ALU / 移位 / 比较指令
```

### 关键根因剖析

1. **缺失直接块链接 (Block Chaining)**：
   每一次条件分支或循环跳转（如 LZSS 匹配查找的紧凑内层循环），即使目标基本块早已编译并常驻在 Translation Cache 中，原生代码仍然会执行 `asmCALL_cpu(PPC_STUB_NEW_PC_REL)`，跳出原生代码回到汇编分发器，再次调用 C++ 的 `jitcNewPC`。仅 128KB 压缩就产生了 **2.4 亿次** 不必要的调度循环。

2. **缺失 Code TLB Fast Path**：
   在 `jitc_mmu.S` 中，`ppc_effective_to_physical_code` 汇编桩没有实现任何 TLB 缓存查找，而是无条件直接调用 C++ 函数 `ppc_effective_to_physical`。这意味着每次跳转都必须走一遍 IBAT 线性扫描甚至 2 级 PPC 页表散列，占用了总运行时间的一半（~50%）。

3. **零动态寄存器分配 (Zero Dynamic GPR Allocation)**：
   AArch64 拥有 31 个 64 位通用寄存器，但目前指令生成（`ppc_alu.cc` / `ppc_mmu.cc`）采用的是全内存往返策略：
   ```asm
   ldr  w16, [x20, #gpr_rA]
   ldr  w17, [x20, #gpr_rB]
   add  w16, w16, w17
   str  w16, [x20, #gpr_rD]
   ```
   每条 PPC 指令都产生 2~3 次对 `gCPU` 结构体的物理读写，流水线受内存读写延迟（Load-to-use penalty）严重制约。

4. **Data TLB 容量受限 (32-entry Direct Mapped)**：
   `TLB_ENTRIES` 仅为 32 项，直接映射（5 位哈希）。在复杂负载下极易冲突，导致频繁跌入 C++ 慢速页表遍历。

5. **AltiVec (VMX) 完全解释执行**：
   所有向量算术与访存指令被硬编码路由到 `ppc_opc_gen_interpret`。Mac OS X 高度依赖 AltiVec 加速界面渲染与基础库函数，向量指令解释执行会造成显著卡顿。

6. **视频显示转换未向量化**：
   `src/system/arch/generic/sysvaccel.cc` 使用标量三重循环逐像素处理颜色格式转换，缺少针对 ARM NEON 的汇编/内联向量加速。

7. **构建参数未内联**：
   `configure.ac` 在默认情况下注入了 `-fno-inline`，阻止了编译器自动内联关键热点工具函数。

---

## 3. 分阶段优化实施路线图

```
┌─────────────────────────────────────────────────────────────┐
│ 阶段一：低成本、高收益快速见效 (Low-Hanging Fruits)            │
│  - Code TLB 汇编快路径 (消除 ~40% 纯开销)                      │
│  - 构建参数优化 (移除 release 模式下的 -fno-inline)            │
│  - 自动化回归测试修复 (macOS timeout 兼容)                    │
├─────────────────────────────────────────────────────────────┤
│ 阶段二：JIT 核心跳转与调度重构 (JIT Core Overhaul)           │
│  - 直接块链接 (Direct Block Chaining): 分支直跳原生代码        │
│  - 同页分发快路径汇编内联 (消除 jitcNewPC 繁重查表)            │
├─────────────────────────────────────────────────────────────┤
│ 阶段三：寄存器分配与访存优化 (RegAlloc & Memory)              │
│  - 基本块内寄存器分配 (激活 X9-X15, X21-X28 映射)             │
│  - 扩大 Data TLB (提升至 128/256 项或 2-way 组相联)           │
├─────────────────────────────────────────────────────────────┤
│ 阶段四：向量与显示架构优化 (Vector & System Architecture)    │
│  - AltiVec 指令原生 JIT 发射至 ARM NEON (V0-V31)             │
│  - NEON 加速的 sysvaccel 帧缓冲颜色转换                       │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. 阶段一优化实施细节

### 4.1 指令 TLB 快路径 (Code TLB Fast Path)
- **数据结构**: `PPC_CPU_State` 内已预留 `tlb_code_eff[TLB_ENTRIES]` 与 `tlb_code_phys[TLB_ENTRIES]`。
- **汇编查表**:
  在 `jitc_mmu.S` 的 `ppc_effective_to_physical_code` 开头增加快路径：
  1. 提取有效地址的高位 Page Tag 与低位 Page Offset。
  2. 根据 `(EA >> 12) & (TLB_ENTRIES - 1)` 计算索引。
  3. 对比 `tlb_code_eff[index]` 是否匹配。
  4. 命中时直接计算 `PA = tlb_code_phys[index] | (EA & 0xFFF)` 并 `ret` 返回，耗时仅 ~6 条指令。
- **慢速回填**:
  在 `ppc_effective_to_physical_code_c` 中，当 `ppc_effective_to_physical` 成功且目标属于物理内存时，回填对应索引的条目。
- **失效保证**:
  `ppc_mmu_tlb_invalidate_all_asm` 与 `ppc_mmu_tlb_invalidate_entry_asm` 已具备重置 `tlb_code_eff` 的逻辑，行为完全安全一致。

### 4.2 编译内联优化
- 修改 `configure.ac`，当未明确指定 `--enable-debug=yes` 时，默认使用非 debug 的标准 release 构建（去掉 `-fno-inline`），使 Clang 的 `-O3` 能够正常内联热点辅助例程。

---

## 5. 阶段二及后续规划要点预研

### 5.1 直接块链接 (Direct Block Chaining)
- PPC 条件分支与无条件跳转的目标往往是确定的。当基本块 A 执行完时，若目标基本块 B 已被翻译：
  - 在 AArch64 下，无条件分支指令 `b <offset>` 支持 ±128 MB 的跳转范围。
  - 由于 Translation Cache 大小为 64 MB，任意两块之间均可直接用单条 `B` 指令互联。
  - 在 macOS ARM64 上，修改已执行代码需要调用 `pthread_jit_write_protect_np(0)`，改完后切回 `(1)` 并用 `__builtin___clear_cache` 刷新。由于块链接只需一次性打补丁（Patching），后续成千万次循环都将以零开销纯硬件速度直跳。

### 5.2 基本块内寄存器分配
- 优先选择调用频率最高的 PPC 寄存器：
  - `r1` (PPC Stack Pointer) -> 固定映射到 `X21`
  - `r2` / `r13` (TOC / SDA) -> 固定映射到 `X22` / `X23`
  - `r3` ~ `r10` (参数与返回值) -> 块内使用 `X9` ~ `X15` 进行 LRU 暂存
- 块末尾或遇到函数调用时执行 Dirty Register Flush，预计可减少 60% 以上的 LDR/STR 指令发射。
