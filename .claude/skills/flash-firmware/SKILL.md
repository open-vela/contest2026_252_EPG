---
name: flash-firmware
description: BK7258 固件的构建→CRC 打包→烧录→真机验证闭环。触发词：烧录、刷固件、烧板子、flash、下载固件、编译烧录、重烧、刷回产品镜像、板子烧不进去、复位窗口。
---

# BK7258 构建烧录闭环

四步缺一不可：**构建 → CRC 打包 → 烧录 → 真机验证**。跳过打包烧进去的镜像
**一定不启动**，而且现象像板子坏了。

## 完整流程

```bash
# 1. 构建 —— 必须在 openvela 工作区根目录执行
cd <workspace-root>          # 不是 contest2026_*/，不是 board/
./build-macos.sh vendor/openvela/boards/contest2026_252_board/configs/nsh

# 2. CRC 打包 —— 不能省
/opt/homebrew/bin/python3 contest2026_252_EPG/board/contest_board/tools/bk_crc_pack.py \
  cmake_out/contest2026_252_board_nsh/nuttx.bin nuttx_crc.bin

# 3. 烧录到 app 分区物理偏移 0x11000
/opt/homebrew/bin/python3 contest2026_252_EPG/board/contest_board/tools/bk_flash.py \
  nuttx_crc.bin 0x11000 /dev/cu.usbserial-10 --reboot

# 4. 真机验证（见 serial-verify skill）
```

参数：`bk_flash.py <镜像> <起始地址> [串口] [--reboot]`；
读回用 `bk_flash.py --read <输出文件> <起始> <长度> [串口]`。

## 六个每次都可能绊倒人的点

### 1. 构建脚本必须从工作区根目录调用

`build-macos.sh` 在工作区根，不在参赛仓里。`cd` 进子目录再调它会报
`no such file or directory`——**但如果你是用 `cmd1 && cmd2` 串起来的，
前一条的 `cd` 会让后一条静默跑错地方**。

⚠️ 更阴的情况：脚本没跑，但你的 `grep -c error` 得到 0，看起来像"构建成功"。
**判断构建成功要看产物存在**，不要看错误数：

```bash
ls -la cmake_out/contest2026_252_board_nsh/nuttx.bin
```

### 2. 加了新 CONFIG 必须 distclean

cmake 只在**初次配置**时把 defconfig 展开成 `.config`，之后的 `olddefconfig`
拿的是已有 `.config`，**新加的行被静默无视，构建照样成功**。

```bash
rm -rf cmake_out/contest2026_252_board_nsh    # 然后重新构建
grep '^CONFIG_你的新开关=y' cmake_out/contest2026_252_board_nsh/.config
```

最后那行 grep 不是可选的。

### 3. CRC 打包不是可选步骤

BK7258 的 flash 控制器在 XIP 取指时做 CRC 校验：每 32 字节插 2 字节 CRC，
物理:虚拟 = 34:32。**未打包的镜像 CPU 取指就是乱码**。

### 4. 烧录需要两个外部依赖

| 依赖 | 位置 | 缺了会怎样 |
| --- | --- | --- |
| `bk_loader` | `/usr/local/bin/bk_loader` | `bk_flash.py` 报找不到命令 |
| pyserial | **只有 `/opt/homebrew/bin/python3` 有** | `ModuleNotFoundError: No module named 'serial'` |

系统默认的 `python3` **没有** pyserial，一定要写全路径。

### 5. `--reboot` 是必须的，不是方便功能

bootrom 的下载窗口只有几十毫秒。"进程 A 发 reboot → 退出 → 进程 B 启动烧录器"
这种做法，**进程接缝本身就宽于整个窗口，从来不会成功**。

`--reboot` 把 reboot 塞进烧录器内部：在**同一个已打开的串口**上发出 reboot，
下一微秒开始 LinkCheck 轰炸，零间隙。实测连续全自动成功，探测数 3300+ 稳定。

> 历史教训：几周里所有"reboot 刷机成功"其实都是操作者恰好按了 RST，归因错误
> 一直没被戳穿。**旁边有人能按按钮时，"成功"必须核对归因。**

### 6. DTR/RTS 无响应不能当死机证据

本板 CEN 没接 CH340 的控制线，硬件流控**永远**无效。串口设置固定
**115200 8N1、无流控**。烧录时的 1500000 是 bootrom 协商的传输波特率，
与控制台波特率无关，别混。

## 两个配置，别混用

| 配置 | 用途 | flash |
| --- | --- | --- |
| `configs/nsh` | 产品镜像（演示用） | 45.46% |
| `configs/xts` | 验证镜像（跑必测集，剥掉 BLE 与演示 app） | 60.40% |

**跑完必测把 `nsh` 烧回去。**

## 烧录后必须验证

烧进去 ≠ 跑起来。至少确认串口有输出、关键 `/dev` 节点在。**改了驱动就跑一次
对应的诊断命令**（`batttest` / `timertest` / `flashtest` / `bkreg`），
不要只看"启动了"。

## 出厂固件备份

动 flash 之前先备份，恢复用同一个工具：

```bash
bk_flash.py --read backup.bin 0x0 0x800000 /dev/cu.usbserial-10
```

⚠️ **上边界 `0x7fe000` 开头是 `"TLV"` 出厂射频校准数据，BLE 依赖它，
不要覆盖。**
