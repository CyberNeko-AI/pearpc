# OS X 10.2 硬盘启动修复与性能记录（2026-09-08）

## 结论

硬盘启动时的禁止通行符号已通过模拟器修复解除；用户确认能够进入系统。
根因是 Open Firmware 设备树中 `mac-io` 的 `compatible` 属性长度少算一个字节。
修复保留在 `src/io/prom/promdt.cc`，临时使用的客体函数追踪代码已经移除，根目录 `make -j4` 构建通过。

## 证据与修复

原代码：

```cpp
macio->addProp(new PromPropMemory("compatible", "paddington\0heathrow", 19));
```

字符串列表需要 20 字节：`paddington` 的 10 字节、分隔 NUL、`heathrow` 的 8 字节、结尾 NUL。
原实现只复制 19 字节。硬盘启动的客体内存实际出现：

```text
paddington\0heathrowte\0_ipc_objec...
```

Darwin 的 `IODTCompareNubName` / `CompareKey` 使用 `strlen()` 遍历属性内的字符串。
因此第二项被读成 `heathrowte`，不能匹配 AppleHeathrow 的 `IONameMatch = heathrow`。
这一行为可与 [XNU 344 的 IODeviceTreeSupport.cpp](https://github.com/apple-oss-distributions/xnu/blob/xnu-344/iokit/Kernel/IODeviceTreeSupport.cpp) 对照。

后果链：

1. BootX 已装入 AppleHeathrow、CMD646ATA、IOATAFamily 等驱动文件，但装入文件不代表驱动已匹配并启动。
2. CMD646ATA 的设备扫描、双缓冲分配、工作循环和 DMA 通道分配均成功。
3. `createDeviceInterrupt()` 进入 `lookUpInterruptController()`，等待 `IOInterruptController00000011`。
4. Heathrow 未成功匹配，中断控制器未注册，CMD646ATA 尚未发布磁盘设备，因而没有 `IDE-CMD`。
5. 内核等待 `IODeviceTree:.../ata-4/@0:9`，无法挂载根分区。

修复用数组大小避免再次手工算错长度：

```cpp
// Include the final NUL: Darwin compares every compatible entry with strlen().
const char macioCompatible[] = "paddington\0heathrow";
macio->addProp(new PromPropMemory("compatible", macioCompatible, sizeof macioCompatible));
```

为何原来光盘能启动：缺少终止符后的读取依赖相邻内存内容，两种 BootX 加载路径的内存布局不同。
光盘成功不能证明这个属性格式正确；布局不同导致匹配结果不同，是结合修复前后实验与内存证据的解释。

## 验证结果

使用 Apple Silicon aarch64 JIT、512 MiB 客体内存和磁盘镜像的 APFS 副本运行测试。
原始安装镜像未用于自动化写入测试；只读挂载的副本已经卸载，测试进程已经停止。

| 测试 | 结果 |
| --- | --- |
| 修复前硬盘启动 | 设备扫描后无 IDE 命令，等待根设备 |
| 修复后硬盘启动，带临时追踪 | IDENTIFY、SET FEATURES、READ DMA 出现；挂载 `disk0s9`，运行启动脚本 |
| 移除临时追踪后的最终构建，硬盘启动 | 再次挂载 `disk0s9` |
| 最终构建，硬盘＋光驱，显式从光盘启动 | 挂载 `disk1s1s9` |
| 最终构建，仅从盘光驱 | 出现 IOSCSIMultimediaCommandsDevice 数据访问异常；尚无相同配置的修复前对照，不能判定是否为本补丁回归 |
| 用户实际硬盘启动 | 用户确认成功进入系统 |

硬盘内核日志关键证据：

```text
Got boot device = IOService:/GossamerPE/.../CMD646ATA/ATADeviceNub@0/IOATABlockStorageDriver/.../IOApplePartitionScheme/未命名@9
BSD root: disk0s9, major 14, minor 9
devfs on /dev
Jettisoning kernel linker.
```

光盘对照日志关键证据：

```text
Got boot device = IOService:/GossamerPE/.../CMD646ATA/ATADeviceNub@1/IOATAPIProtocolTransport/.../IOApplePartitionScheme/Mac_OS_X@9
BSD root: disk1s1s9, major 14, minor 18
```

`Warning: AppleMacIO self test fails` 在能挂载根分区的两种启动中都存在，不能单凭它判定本次修复失败。

## 历史研判纠正

- SET FEATURES 命令会走到公共的中断发送路径，不能因 case 内没有 `raiseInterrupt()` 就认定缺失中断。
- `IO_PIC_IRQ_IDE0` 已是 26（十六进制 `0x1a`），二者不是不同 IRQ。
- 原有桥接器中断映射应保留；给 PCI ATA 增加直接指向 PIC 的 `interrupt-parent`，同时保留 PCI 引脚编号 `interrupts = 1`，会改变解析语义。
- 早期逐设备打印的 `PCI-IO-MISS` 不代表整个 PCI 总线未命中；还要检查后续设备是否处理请求。
- 本次故障发生在磁盘设备发布之前，不能把 IOATABlockStorageDriver 未出现直接当作该驱动自身损坏。

## 硬盘启动为什么比光盘慢

现在确认了启动路径差异，但还没有做关闭日志、相同终点的分段计时，不能声称某项占用多少秒或多少百分比。

### 已观察到的差异

| 指标 | 硬盘测试 `hd-final.log` | 光盘测试 `cd-fixed-both.log` |
| --- | --- | --- |
| BootX 独立 `Driver-*` 加载记录 | 108 | 0 |
| BootX `DriversPackage-*` 加载记录 | 0 | 1 |
| 首条 IDE 命令前的 `[IO/PROM]` 日志条数 | 55,837 | 2,092 |

光盘内存中在本次运行的物理地址 `0x45d000` 找到 `MKXTMOSX` 缓存包头，日志也有 `DriversPackage-45d000`。
硬盘镜像只读检查时没有 `System/Library/Extensions.mkext`，对应日志显示逐个装入 108 个驱动。
后续正常关机是否自动生成缓存尚未检查；以上是测试时的状态。

逐个扫描目录、解析 plist、打开驱动文件会增加 BootX 执行和 PROM 调用开销。
[Apple BootX 的驱动加载代码](https://github.com/apple-oss-distributions/BootX/blob/BootX-59/bootx.tproj/sl.subproj/drivers.c) 说明了缓存包与目录加载两条路径。
这支持优先排查早期驱动加载开销；日志条数不等于执行时间，也不代表同等倍数的减速。

关闭调试输出之前的构建还存在以下开销：

- `src/debug/tracers.h` 开启 `IO_PROM_TRACE`。
- `src/io/ide/ide.cc` 的 IDE-CMD、IRQ、SEL 和 BMIDE 诊断大量直接调用 `ht_printf()`，不受关闭 `IO_IDE_TRACE` 控制。
- 硬盘启动进入完整安装系统，运行启动服务并执行写入；安装光盘进入安装环境，两者不是相同工作负载。
- 上一轮对照测试进程曾与用户的模拟器同时运行，可能造成 CPU 竞争；现已停止该测试进程。未量化这部分影响。

硬盘已有 `cmd=0xc8` 的 DMA 读与 `cmd=0xca` 的 DMA 写；不能把速度差归因为没有启用 DMA。
`ATADeviceFile` 已尝试 mmap，DMA 已按 PRD 批量搬运。尚无证据表明宿主磁盘吞吐是主要瓶颈。

### 下一步性能测试方案

1. 一次只运行一个实例，固定 CPU 后端、内存、显示模式和镜像；正常关机后重复启动，区分首次启动与后续启动。
2. 将高频 PROM／IDE／JIT 日志做成默认关闭的诊断选项，保持错误日志；分别测试开启与关闭，避免把输出到终端的耗时算作磁盘性能。
3. 记录四个时间点：BootX 开始、内核开始、`BSD root`、桌面或安装界面可用。先比较两种启动的前三个时间点。
4. 在 Jaguar 客体内核实并生成适配该系统的驱动缓存，再确认下次启动出现 `DriversPackage-*`。不要把安装光盘的缓存直接复制到硬盘安装系统。
5. 若 `BSD root` 之后仍慢，再采样 JIT CPU 时间、I/O 请求大小和启动服务等待时间。现阶段不继续试改中断和 ATA 命令语义。

上述表格来自关闭调试输出之前的测试；客体驱动缓存尚未由本轮主动修改。

### 后续实施：默认关闭调试输出

按用户要求，已将 `PEARPC_DEBUG_TRACE` 默认设为 `0`，统一关闭原来默认开启的 TRACE 宏，
以及直接打印的 PROM、IDE/BMIDE、中断、DEC/SPR、JIT 调度、显卡和 SDL 诊断。
分区扫描打印和 PROM 写数据预览也不再默认执行。错误、警告、启动菜单以及客体自身的控制台输出保留。

另修复一个与日志相关的性能问题：aarch64 JIT 即使 `jitc_log_file` 为空，之前仍会反汇编 PPC 和
AArch64 指令、查符号并格式化字符串，随后才发现没有日志文件。现在在格式化之前返回。
显式指定 `jitc_log_file` 时，仍保留反汇编日志功能。

验证包括根目录 `make -j4`、关闭／开启 TRACE 的参数求值及警告检查、开启诊断分支的语法检查，
以及使用硬盘副本的启动回归；关闭高频日志后仍确认 `BSD root: disk0s9`。
这次测试不作为启动耗时基准，也未修改 ATA、中断或 CPU 指令的执行语义。

正常运行无需新增配置。以后如需恢复诊断输出，可在仓库根目录重新编译：

```sh
make clean
make -j4 CPPFLAGS="-DPEARPC_DEBUG_TRACE=1"
```

改变编译选项后需要清理旧目标文件，避免混用。恢复安静构建：

```sh
make clean
make -j4
```

此开关独立于 `--enable-debug` 的编译调试符号；不会自动开启逐指令文件追踪。

## 重建与复测

从仓库根目录执行 `make -j4`，确保设备归档库也重建，然后运行 `./src/ppc ppccfg.osx`。
若同时接有光盘，使用 `prom_env_bootpath = "disk0:9"` 明确选择硬盘；当前用户配置关闭从盘光驱时无需此项。
诊断时可设置 `prom_env_machargs = "-v"`，确认 `BSD root: disk0s9`。
不要把历史日志中仅出现设备扫描视作挂载成功，或把本轮日志条数当作正式性能基准。
