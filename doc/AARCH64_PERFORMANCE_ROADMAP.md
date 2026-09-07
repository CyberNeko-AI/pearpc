# PearPC on AArch64 (Apple Silicon): 运行效率评估与优化路线图

本文档记录了 PearPC 在 Apple Silicon (macOS arm64) 架构下的性能现状评估、基于底层采样的热点分析根因诊断，以及系统性的分阶段优化演进路线图。

---

## 1. 运行效率现状与基准数据

### 1.1 基准测试环境
- **Host 硬件**: Apple Silicon (arm64, macOS Darwin)
- **模拟器目标**: PowerPC G4 (7400/7410, PVR 0x000c0201)
- **编译配置**: `./configure --enable-ui=sdl --enable-release`
- **基准测试程序**: `test/test_bench.elf` (128 KiB 数据量，包括 xorshift32 PRNG、MD5 完整性哈希、LZSS 字典压缩与解压比对)

### 1.2 性能演进与对比
 
| 阶段 / 版本 | 执行耗时 (User Time) | 相对 Baseline 提速 | 调度方式 / 核心瓶颈 |
| :--- | :---: | :---: | :--- |
| **Host 原生 C (clang -O3)** | ~0.07 秒 | 61.1x | 本地机器码直接运行 |
| **原始基线 (Baseline)** | 4.28 秒 | 1.0x (基准) | 无 TLB 汇编快路径、全 C++ 调度 (`jitcNewPC`) |
| **阶段一优化后** | 1.80 秒 | **2.38x** | 汇编级 Code TLB Fast Path + Release 构建内联 |
| **阶段二优化后** | **0.37 秒** | **11.57x** (耗时 -91.3%) | 汇编直接入口查找 + 同页分支直跳 (Block Chaining) + 内联分发快路径 |
 
> 注：所有阶段均通过 `test/run_tests.sh` 包含的 12 项全套回归测试，行为与结果 MD5 完全一致。

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

## 5. 阶段二优化实施细节
 
### 5.1 汇编直接入口查找 (Assembly Entrypoint Fast Lookup)
- **背景**: 在原实现中，每次分发都要调用 C++ 的 `jitcNewPC(jitc, pa)`，执行繁重的 LRU 链表更新与 `ClientPage` 查找。
- **优化**: 在 `jitc_tools.S` 的 `ppc_new_pc_asm` 中，直接用汇编读取 `clientPages[PA >> 12]` 与 `entrypoints[(PA & 0xFFF) >> 2]`。命中时直接 `br x5`，将 C++ 函数调用由每秒数千万次降为几乎为零。
 
### 5.2 同页直接分支与块链接 (Direct Intra-Page Branch / Block Chaining)
- **CFG 块入口自动登记**: 在 `jitc.cc` 的 `jitcNewEntrypoint` 指令翻译循环中，利用 `PageCFG` 识别出的所有基本块入口（包括所有分支目标与循环头），在首次翻译到达时即记录其原生机器码地址至 `cp->entrypoints[ofs >> 2]`。
- **无开销原生跳转**: 在 `ppc_alu.cc` 的 `bx` 和 `bcx` 指令生成中：
  - 若目标在同页且原生机器码地址已生成（`entrypoints[targetOfs >> 2] != 0`，覆盖 100% 的循环回跳）：
  - 生成原生条件跳转：
    ```asm
    ldr     w16, [x20, #exception_pending]  // 检测异步中断/定时器
    cbnz    w16, .Lslow_fallback            // 有未决中断时走完整分发
    b       <target_native_address>         // 纯硬件直跳循环头！
    .Lslow_fallback:
    mov     w0, #targetOfs
    call    ppc_new_pc_rel_asm
    ```
  - 使计算密集型循环完全留在 CPU 硬件流水线中高速执行，彻底消除了循环体跳板开销与分支预测阻滞。
 
### 5.3 分发跳板内联化 (Inlined Heartbeat & Code TLB in `ppc_new_pc_asm`)
- 将 `ppc_heartbeat_ext_asm` 与 `ppc_effective_to_physical_code` 的快路径直接内联展开至 `ppc_new_pc_asm` 中，消除了跨函数的 `bl`/`ret` 栈帧与调用开销。
 
---
 
---

## 6. 阶段三优化实施细节

### 6.1 页内前向分支反向修补与全量块链接 (Direct Block Chaining with Backpatching)
- **背景**: 阶段二实现了对已知目标（主要是循环回跳）的原生直跳，但对于所有前向分支（如循环内的 `while`/`if` 条件跳出），由于目标块尚未编译，先前直接退回到 `PPC_STUB_NEW_PC_REL`，且在目标块编译后从未回填修补。这导致热点内层循环每次条件不满足跳出时，都要通过慢速分发桩走完整的 Code TLB 查询。
- **优化**:
  - 在 `ClientPage` 中增加挂起分支修补表 `BranchFixup fixups[MAX_PAGE_BRANCH_FIXUPS]`。
  - 对于所有同页分支，统一发射 24 字节结构：
    ```asm
    ldr     w16, [x20, #exception_pending]  // 心跳/中断检测
    cbnz    w16, +8                         // 有异常跳过直跳
    b <target> / nop                        // 已知则直跳，未知则发射 nop 占位符
    mov     w0, #targetOfs
    call    PPC_STUB_NEW_PC_REL
    ```
  - 当目标块随后被翻译（`jitcCreateEntrypoint`）时，立即就地将挂起的 `nop` 指令覆写修补为 `a64_B(target_native - branch_site)`，并经由 `jitcFlushClientPage` 统一清空指令缓存。
  - **收益**: 循环内所有前向分支在目标被翻译后**永久变为单条硬件直跳**。在性能采样分析中，`ppc_new_pc_asm` 由原先的 41% 占比直接彻底降为 **0 次采样（完全消失）**！

### 6.2 MMU TLB 扩容与汇编索引指令精简
- **TLB 扩容至 64 项**: 将 `TLB_ENTRIES` 从 32 提升至 64。在 64 项下，`tlb_data_8_phys` 在 `PPC_CPU_State` 中的最大偏移为 3204 字节，仍然完美保持在 AArch64 单指令立即数寻址范围（< 4096）内。
- **`ubfx` 指令精简**:
  在 `jitc_mmu.S` 和 `jitc_tools.S` 中，将所有内存读写桩与分发查找中的双指令操作：
  ```asm
  lsr     w2, w0, #12
  and     w2, w2, #(TLB_ENTRIES - 1)
  ```
  精简为单条 AArch64 无符号位域提取指令：
  ```asm
  ubfx    w2, w0, #12, #TLB_BITS
  ```
  显著缩短了每个访存快路径的关键路径延迟。

---

## 7. 各阶段性能实测对比汇总

测试平台: Apple Silicon (M系列 macOS arm64), 基准测试: `test/test_bench.elf` (128 KiB 数据量, PRNG + MD5 + LZSS 压缩解压):

| 阶段 | 关键改动 | 执行耗时 | 相对基线提速 |
|---|---|---|---|
| **Baseline** | 初始状态 (Generic / aarch64 JIT) | **4.28s** | 1.00x |
| **阶段一** | Code TLB 汇编快路径 + Release 编译内联 | **1.80s** | 2.38x |
| **阶段二** | 汇编直接入口查表 + 循环后向直跳 | **0.37s** | 11.57x |
| **阶段三** | 全量块链接反向修补 + TLB 扩容至 64 + ubfx 指令压缩 | **0.23s** | **18.61x** |

累计耗时削减：**94.6%**（从 4.28 秒骤降至 0.23 秒）！
回归测试：`./test/run_tests.sh` 全部 12 项测试保持 100% PASS。
