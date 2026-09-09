# PearPC 网络功能现状（macOS）

## 当前实现

PearPC 已包含 3Com 3C90x 和 Realtek RTL8139 网卡模拟。两者都通过
`createEthernetTunnel()` 连接宿主机网络后端，相关代码位于：

- `src/io/3c90x/3c90x.cc`
- `src/io/rtl8139/rtl8139.cc`
- `src/system/osapi/posix/sysethtun.cc`

macOS 分支使用旧式 TUN 设备，并固定打开 `/dev/tun0`。因此仅在配置文件中启用
网卡并不足以联网；宿主机必须提供可读写的 TUN 设备。

## macOS 限制

当前系统未提供 `/dev/tun0`。现代 macOS 已移除早期系统中的 `tunnel.kext`，而
PearPC 代码仍假定该设备存在。启用网卡后，初始化通常会因无法打开设备而失败。

可以用下面的命令确认环境：

```sh
ls -l /dev/tun0 /dev/tap0
```

## 配置入口

`ppccfg.example` 中已有网卡开关和 MAC 地址，例如：

```ini
pci_rtl8139_installed = 1
pci_rtl8139_mac = "52:54:00:12:34:56"
```

这只会启用客户机中的 RTL8139，不会创建宿主机网络设备。

## 后续方案

推荐为 macOS 增加用户态网络后端（例如 UDP/NAT），避免依赖废弃的 TUN 驱动。
实现时需要：

1. 在 `EthTunDevice` 抽象层增加 NAT/UDP 后端；
2. 将 RTL8139 收发的以太网帧转换为宿主机套接字流量；
3. 增加配置项选择后端，例如 `pci_rtl8139_network = "nat"`；
4. 保留现有 TUN 路径，供安装了兼容驱动的系统使用；
5. 增加 DHCP、ARP、IPv4 连通性和关闭清理测试。

TUN 是三层接口，而 RTL8139 工作在二层；若需要完整以太网帧转发，应使用 TAP
或在用户态实现 NAT，而不是只创建一个三层 TUN 接口。
