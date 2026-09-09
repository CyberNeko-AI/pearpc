# Mac OS X 10.2 蓝色背景故障：跨页多寄存器访存丢失异常 PC

## 结论

已定位并修复 AArch64 JIT 的 `lmw/stmw` 异常位置错误。修复前，启动脚本的 `/bin/sh` 在跨页存储时收到错误的异常返回地址，随后崩溃；NetInfo、Directory Services 等启动项无法完成，首次设置助手停留在空白蓝色背景。修复后的硬盘副本已显示中文 Setup Assistant「欢迎」界面。

这里验证到的是首次设置界面恢复；还没有填写用户资料、创建账户或验证设置完成后的 Finder 桌面。

## 证据与定位

硬盘中的 `/private/var/log/system.log` 表明 WindowServer 已成功映射 800×600 显示器。同时存在：

```text
crashdump: Crash report written to: /Library/Logs/CrashReporter/sh.crash.log
SystemStarter: Network extensions (...) did not complete successfully.
DirectoryService: NetInfo timeout connecting to local domain, sleeping
Setup Assistant: DirectoryService Framework::CMessaging::Mach msg send interrupt error = -14006.
```

`sh.crash.log` 的故障地址随启动变化，但调用栈、`lr=0x00010738` 和其他寄存器重复。仅凭这些日志，不能确定是 `blr/bctr` 计算目标错误，也不能排除其他 CPU 或客户机问题。上一轮 25 秒通用解释器运行没有形成相同启动阶段的对照，因此不能据此认定 JIT 特有故障。

本轮在硬盘镜像与 NVRAM 副本上添加临时运行时跟踪，获得直接证据：

```text
[SH-DSI] pc=04f54000 ofs=04f46000 ccb=0000e000 dar=bffff000 flags=0a000000 sp=bffff030
[USER-ISI] ea=04f54000 pc=04f54000 ... lr=00010738 ctr=900044a0 ...
```

从硬盘提取 `/bin/sh` 并按 Mach-O `__TEXT` 的虚拟地址和文件偏移反汇编：

```text
00010734: bl    0x0000eff8
0000eff8: stmw  r15,-68(r1)
0000effc: mfcr  r2
```

当 `r1=0xbffff030`，这条 `stmw` 写入 `0xbfffefec` 至 `0xbffff02c`，跨越 `0xbffff000` 页边界。后一个页面触发写保护 DSI。正确的 `SRR0` 应为 `0x0000eff8`，实际却是 `0x04f54000`；内核从异常返回后在该错误地址触发 ISI，最终杀死 shell。

## 根因和修改

`src/cpu/cpu_jitc_aarch64/ppc_mmu.cc` 将 `lmw/stmw` 展开为多次单字访存。每次访存通过 `W9` 向汇编辅助函数传入当前 PPC 指令在页内的偏移。慢速路径再计算：

```text
PPC fault PC = current_code_base + W9
```

原实现仅在循环开始前设置一次 `W9`。AArch64 C 调用可以覆盖这个调用者保存寄存器：第一个页面的 TLB miss 即使成功返回，也可能覆盖 `W9`；后续页面故障便把覆盖后的值保存为 `SRR0`。

修复将 `gen_prologue(jitc)` 移到 `lmw` 和 `stmw` 循环内部，在每次调用访存辅助函数前重新设置偏移。最终修改没有保留针对 `sh` 地址的跟踪，也没有禁用间接分支优化。

## 回归测试

新增 `test/test_multiple_dsi.S`、配置与预编译 ELF，并加入 `test/run_tests.sh` 和 `test/build_ppc_elf.sh`。

测试分别为 `stmw` 和 `lmw` 准备两个连续页面：

1. 第一个页面有有效 PTE，但 TLB 已失效，保证第一次访问走成功的慢速路径。
2. 多字访问跨入尚未映射的第二个页面，触发 DSI。
3. 自定义异常处理程序保存实际 `SRR0`、`DSISR` 和异常次数，建立映射并重试。
4. 检查异常位置、读写类型和所有传输数据。处理程序仅在测试中修正返回地址，使错误版本明确报错而非失控跳转；实际捕获的错误 `SRR0` 仍使检查失败。

最初实验样例误以为客户机可直接访问页表的物理地址，样例自身引入了额外 DSI；最终测试改为使用独立页面，消除了这一测试环境错误。

```sh
./src/ppc --headless test/test_multiple_dsi.cfg
bash test/run_tests.sh
```

旧二进制运行新测试：退出码 `100`（异常 PC 不匹配）。修复后：退出码 `0`。完整套件：`13 passed, 0 failed, 0 skipped`。

## 客户机启动验证

使用当前 `ppccfg.osx` 的 G4、512 MiB、800×600 配置，测试介质和 NVRAM 均使用 `/tmp/pearpc-desktop-fix/` 下的副本；没有修改原始硬盘安装内容。

* 硬盘启动：运行 100 秒后捕获帧缓冲，已显示首次设置欢迎页。
* 最新一次启动日志出现 `Starting NetInfo`、`Starting Directory Services`、`Starting Core Services`；该次启动未新增 `sh.crash.log` 记录，也未再出现原先的 NetInfo 超时链。
* 测试结束时由脚本发送 `SIGTERM` 捕获帧缓冲。宿主日志中的 SIGTERM 和退出码 143 是测试主动结束，不是客户机崩溃。

四种自动启动介质组合均已验证：

| 介质组合 | 本轮观察结果 |
| --- | --- |
| 只有可启动硬盘 | 进入首次设置欢迎页，国家选择和继续按钮正常显示 |
| 只有可启动光盘（安装盘 1） | 进入 Installer 语言选择窗口，菜单栏正常显示 |
| 可启动硬盘＋不可启动光盘（安装盘 2） | 回退到硬盘，进入首次设置欢迎页 |
| 不可启动硬盘（新建空白镜像）＋可启动光盘 | 从安装盘 1 进入 Installer 语言选择窗口，菜单栏正常显示 |

后三组分别运行 65 秒并捕获帧缓冲，宿主日志均未出现 `FATAL` 或 `can't open boot file`。本轮验证到首次设置或安装器图形界面；没有代填用户账户，也没有完成首次设置后验证 Finder 桌面。

诊断日志、修复前后测试输出和截图保存在 `/tmp/pearpc-desktop-fix/`。正常 SDL 构建已经完成，可直接运行 `./src/ppc ppccfg.osx` 继续首次设置。

## 后续故障：`stwcx_ SO skip` 宿主断言

继续首次设置后出现 `expected tcp=..., got ... (delta=-17651708)`。这是 JIT 生成代码时的缓存位置断言，不是客户机的异常返回地址问题。

`stwcx.` 原先用 `emitAssure(128)` 预留连续空间，但 `rA != 0` 时实际生成 132 字节，另需保留 4 字节的片段连接指令。当当前片段恰好剩余 132 字节，预留检查通过，末尾更新 SO 的短分支代码却跨入另一个不连续片段。按连续地址计算的目标随即失效。

修复把 SO 更新改为 `LSR` 和 `BFI`，直接把 XER 的第 31 位复制到 CR 的第 28 位，去掉短分支及整段空间预留；其余前向跳转保留可跨片段的宽分支修补。

`test/test_stwcx_fragments.cc` 对实际生成的本机代码做执行验证：当前片段剩余空间从 4 到 512 字节、每次递增 4 字节；下一片段分别位于前后 2 MiB，覆盖两种寻址、SO 清零/置位和三种保留状态，共 3,072 次执行。旧目标文件稳定触发相同断言；修复后全部通过，13 项 PPC ELF 测试也全部通过。用户随后手动确认不再出现该错误。

本轮证据保存在 `/tmp/pearpc-stwcx-fix/`。后续用户报告首次设置中文输入法加载后变慢，快速鼠标运动会伴随 CUDA 超时。宿主 `sample` 采样显示 CPU 线程约 80% 占用，其中约一半样本位于 `jitcNewPC` 的翻译路径，CUDA 事件线程并非主要耗时来源。

性能修复让 word MMU 慢速桩保存/恢复 W9，使 `lmw/stmw` 恢复为每条指令一次序言；DSI 分支为新增的 32 字节栈帧使用独立回收路径。修复后本机代码生成边界测试 3,072 次、PPC ELF 测试 13 项全部通过。CUDA 的 IFR 轮询和事件超时 warning 仍保留为后续可观测性问题；应先重启新构建的模拟器再比较输入法加载和鼠标运动性能。
