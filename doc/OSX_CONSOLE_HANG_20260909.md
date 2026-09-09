# Mac OS X 10.2：视频切换期间的控制台死锁

## 结论

本轮定位并修复了 kext 读取、根分区挂载之后的卡死：缺少 MacIO SCC 串口设备，导致 Darwin 在切换显示模式时通过未初始化的串口基址输出日志，触发内核异常；异常打印又等待已占用的控制台锁，使错误信息无法显示。

修复后的 AArch64 JIT 在四种 IDE 介质组合下都进入图形安装界面。硬盘中的系统尚处于附加软件安装阶段，因此硬盘单启动显示“请插入 Mac OS X Install Disc 2”，并非新的启动失败。尚未以完成系统安装、进入 Finder 桌面作为本轮验收结果。

## 证据与推理

1. 关闭日志的正常构建可以挂载根分区、运行 `/etc/rc`。屏幕最后的 `SystemStarter ... Hangup` 不足以定位最终死锁，更不能直接推断为 IDE 或宿主信号故障。
2. 从实际镜像只读提取 `mach_kernel`，用其符号表解释客体内存转储。最后执行的循环在 `_cnputc + 0xe4`，对应本镜像的 `0x0009d708`：

   ```text
   lwz   r0,0x90dc(r9)
   and.  r11,r28,r0
   bne   0x0009d708
   ```

   `cbfpend` 对应内存 `0x003090dc` 为 1，`sconowner` 为 CPU 0；该 CPU 正在等待自己的控制台缓冲区，MSR.EE 已关闭。先前打印的 `current_opc=7ca00124` 是旧的解释路径状态（`mtmsr r5`），不能把它当作当前 PC 的指令，也不是 DEC 读取指令。
3. 用内核符号 `_hash_table_base`、`_hash_table_size` 取得页表位置，再翻译客体栈地址。调用栈包含：

   ```text
   IOLog("PearPCVideo: vram ...")
     -> cnputc
       -> scc_putc / powermac_scc_set_datum
         -> trap
           -> unresolved_kernel_trap
             -> kdb_printf
               -> cnputc（等待已占用的锁）
   ```

4. 在临时诊断构建中拦截本镜像的 `unresolved_kernel_trap`，直接向宿主 stderr 输出 savearea，不调用客体控制台。得到：

   ```text
   [GUEST-TRAP] type=0000000c dsisr=42000000 dar=00000002
   save_srr0 = 0009df88   # powermac_scc_set_datum
   save_srr1 = 00001030
   save_r3   = 00000000   # 未初始化的 SCC 基址
   save_r4   = 00000002   # channel A control 偏移
   instruction = 7ca321ae # stbx r5,r3,r4
   ```

   `type=0x0c` 是 XNU 内部的异常索引（4 字节间距），此处表示数据访问异常，不是 PPC 的系统调用向量。DSISR 表示存储页错误，故障地址为 `0 + 2`。
5. 同一故障也出现在 generic 解释器基线运行中，因此不属于 AArch64 JIT 独有问题。

上述地址仅对应本次提取的 Jaguar 内核，不可用于其他镜像。临时硬编码诊断钩子已经移除，正式修复不依赖内核地址、不修改客体指令或锁。

## 与 Darwin 源码的对应关系

- [`PE_find_scc`](https://github.com/apple-oss-distributions/xnu/blob/xnu-344.2/pexpert/ppc/pe_identify_machine.c) 仅在设备树发现 `name=escc` 时返回 MacIO 基址加 `0x12000`。
- [`PE_init_kprintf`](https://github.com/apple-oss-distributions/xnu/blob/xnu-344.2/pexpert/ppc/pe_kprintf.c) 根据该地址初始化 SCC。
- [`PE_initialize_console`](https://github.com/apple-oss-distributions/xnu/blob/xnu-344.2/pexpert/ppc/pe_init.c) 在禁用屏幕时临时切到串口控制台，重新启用屏幕时恢复。
- [`serial_io.c`](https://github.com/apple-oss-distributions/xnu/blob/xnu-344.2/osfmk/ppc/POWERMAC/serial_io.c) 的发送路径访问控制寄存器并轮询 TX_EMPTY。
- [`serial_console.c`](https://github.com/apple-oss-distributions/xnu/blob/xnu-344.2/osfmk/ppc/serial_console.c) 的 `cnputc` 缓冲区锁解释了为何第二次打印无法完成。

## 修改

- 在 MacIO 下声明 `escc@12000`，对应 8 字节的旧式 SCC 寄存器窗口。
- 实现两个通道的轮询控制台：寄存器选择、POINT HIGH、复位、波特率寄存器回读，以及立即完成的字节发送。RR0/RR1 返回发送完成状态，不伪造接收数据或待处理中断。
- 新增 `macio_scc_log`。默认空字符串关闭日志；`-` 写 stderr；文件路径写独立串口日志。此设备不依赖 `pci_serial_installed`，后者控制另一种 PCI UART。
- 当前 SCC 范围限于轮询输出；不提供串口输入、DMA 或完整中断驱动串口功能。没有为这些未实现功能声明子通道驱动或中断属性。
- 修正 generic CPU 的字节读取函数链接问题：去掉只定义在 `.cc` 中、但被另一翻译单元调用的 `inline`，使 Clang 优化构建能够链接并运行对照测试。

## 验证

环境：macOS arm64，512 MiB 客体内存，`800x600x15`，`prom_bootmethod="auto"`，`-v`。所有写入硬盘的启动测试使用 APFS 克隆副本，各轮使用独立 NVRAM，原始 `ppccfg.osx` 和镜像没有因本轮测试而修改。

| 场景 | 介质 | 观察结果 |
| --- | --- | --- |
| 仅可启动硬盘 | `osx_hd.img` 的副本 | 75 秒内进入图形“附加软件／请插入 Disc 2”页 |
| 仅可启动光盘 | `osx_10.2_disk1.iso` | 100 秒内进入 Installer 的语言选择页 |
| 可启动硬盘 + 不可启动光盘 | 硬盘副本 + `osx_10.2_disk2.iso` | 从硬盘进入图形界面，并继续执行 Disc 2 附加软件安装 |
| 不可启动硬盘 + 可启动光盘 | 有效大小但没有分区/启动文件的空白 ATA 镜像 + Disc 1 | 回退光盘，100 秒内进入语言选择页 |

补充检查：

- 正常 AArch64 构建成功，`test/run_tests.sh` 的 12 项测试全部通过。
- 新增 `test/test_macio_scc.cc`，覆盖控制台轮询、两个通道隔离、复位、寄存器选择以及实际输出字节，测试通过。
- 使用真实 SDL 窗口、默认关闭 SCC 日志的光盘配置，70 秒内同样进入语言选择页。
- generic 解释器构建、PPC 字符串输出/退出测试通过。修复后的额外完整启动对照遇到 generic 后端已有的除零单步入口（`division by zero @00259cd4`），不能将它记录为 generic 图形启动通过；四种组合的图形验收均由 AArch64 JIT 完成。
- 运行结束时由采集器发送 SIGTERM，日志有 `[HARNESS]` 标记。退出码 143 和其回溯不是客体自行崩溃。

## 重新构建与采集

仓库根目录执行：

```sh
make -j4
./src/ppc ppccfg.osx --macio_scc_log=- 2>&1 | tee osx_boot_console.log
```

命令行覆盖值不会改写配置文件。也可以使用 `--macio_scc_log=osx_guest_serial.log` 把串口字节单独保存。普通运行不需要配置该项，修复始终生效。

此日志包含宿主输出和客体送往 SCC 的内容；已画进帧缓冲的文字不会自动变成串口文字，不能把 stderr 空白等同于没有启动进展。需要完整内核历史时仍应结合内存中的 `msgbuf`；界面状态用 SIGUSR1 帧缓冲转储核验。

## 本轮产物

诊断目录：`/var/folders/wc/cyntbw7s08j9qt85mqgf8cf40000gn/T/pearpc-hang-x8rm79lb/`。

- `trap-console.log`：被控制台死锁遮蔽的内核异常与 savearea。
- `jit-kernel.log`：从基线内存提取的内核日志。
- `scc-hd-console.log`、`cd-only.log`、`hd-data-cd.log`、`blank-hd-boot-cd.log`：四种组合的宿主/串口日志。
- `boot-matrix.png`：四种组合的最终界面并排截图。
- `gui-cd.log`、`gui-cd.png`：默认静默设置下的 SDL 窗口验证。
- `generic-console.log`、`generic-scc-console.log`：解释器修复前/后的独立对照记录。

采集注意：generic 后端目前固定在工作目录打开 `trace_generic.log`。本轮最后的短输出测试在仓库根目录运行，覆盖了根目录原来的这个历史追踪文件；本轮长运行日志和内存转储保存在上述独立目录。后续应从独立工作目录运行 generic 诊断。
