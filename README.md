# openvela on BK7258 —— 声网对话式 AI 开发套件 R1 板级适配

## 参赛信息

| 项目 | 内容 |
| --- | --- |
| 队伍编号 | 252 |
| 队伍名称 | EPG |
| 团队成员与分工 | 曾婷（全部工作：芯片层与板级层移植、驱动开发、真机验证、文档） |
| 选题方向 | 新硬件适配 |
| 作品定位 | 个人桌面 agent —— 会看、会听、会说、会震的桌面机器人 |
| 目标硬件 | 声网对话式 AI 开发套件 R1（Agora ConvoAI Kit R1，Beken BK7258） |
| 真机验证 | 已领板，全部结论均在实板取证（串口日志与真机记录见 PORTING_NOTES.md） |

## 一、作品简介

把 **openvela / NuttX 移植到博通集成（Beken）BK7258** 上，目标硬件是**声网对话式
AI 开发套件 R1（Agora ConvoAI Kit R1）**。BK7258 此前在 openvela 生态中没有任何
支持——`vendor/beken` 是个只有 CI 模板的空仓，`nuttx/arch` 与 `nuttx/boards` 下也
没有相关代码。本作品从零补齐了芯片层（BSP）、板级层、构建集成与烧录镜像打包链路。

亮点：

- **公共仓零改动。** 全部代码在参赛仓内，通过 `CONFIG_ARCH_CHIP_CUSTOM_DIR` /
  `CONFIG_ARCH_BOARD_CUSTOM_DIR` 挂进 openvela 构建树，`nuttx/`、`packages/`、
  `vendor/` 一行未改，完全符合参赛提交规范。
- **寄存器级事实全部有据可查。** 内存映射、60 个中断号、UART 寄存器位域、时钟门控
  位、引脚复用值，逐项来自 BK7258 Datasheet V2.1 与 Beken `bk_idk` SDK 源码，
  README 中标注了出处。
- **反推并验证了 Beken flash 的 CRC 编码格式。** BK7258 的 flash 控制器在 XIP 取指
  时做 CRC 校验（每 32 字节插 2 字节 CRC，物理:虚拟 = 34:32）。厂商的 CRC 工具只有
  Linux 版，我们对 SDK 中随附的已编码镜像做参数穷举，定出算法（MSB-first，多项式
  `0x8005`，初值 `0xFFFF`，大端存储），并在 3 个样本共 **3868 个块上逐块复算，零
  失配**，据此实现了跨平台的 Python 打包工具。
- **在 macOS（Apple Silicon）上打通了完整构建链路。**

## 二、选题方向

**新硬件适配**。BK7258 是国产 Armv8-M（STAR-MC1）三核 AMP 架构的 Wi-Fi 6 + BLE 5.4
多媒体 SoC，openvela 尚未支持，符合该赛道"选择尚未适配的硬件平台"的要求，且属于
评分说明中鼓励的"国产芯片平台"方向。

## 三、目录结构

```
board/contest_board/          板级适配代码（→ vendor/openvela/boards/contest2026_252_board）
├── chip/                     BK7258 芯片层（CONFIG_ARCH_CHIP_CUSTOM_DIR）
│   ├── bk7258_start.c        复位入口：VTOR / FPU / .data / .bss → nx_start()
│   ├── bk7258_irq.c          NVIC 中断控制（60 个外设中断）
│   ├── bk7258_timerisr.c     SysTick 系统节拍
│   ├── bk7258_serial.c       UART 字符设备驱动（/dev/console, /dev/ttyS0）
│   ├── bk7258_lowputc.c      早期调试输出与 UART 线路配置
│   ├── bk7258_clockconfig.c  外设时钟门控与时钟源选择
│   ├── bk7258_gpio.c         GPIO 与引脚复用
│   ├── bk7258_allocateheap.c 堆区划分
│   ├── bk7258_memorymap.h    内存映射与外设基址
│   ├── bk7258_uart.h         UART 寄存器定义
│   └── include/irq.h         中断号定义
├── src/                      板级初始化
├── include/board.h           时钟与引脚约定
├── scripts/ld.script         链接脚本（含 XIP 地址推导过程）
├── configs/nsh/defconfig     最小 NSH 基线配置
├── tools/bk_crc_pack.py      flash CRC 编码 / 校验
├── tools/bk_flash.py         烧录器（持续等待复位窗口，免抢时机）
└── README.md                 详细技术文档（硬件事实表、flash 布局、烧录说明）

logs/                         AI Coding 日志
```

## 四、运行方式

### 1. 拉取工程

```bash
repo init -u https://github.com/open-vela/contest2026_252_EPG \
  -b dev-ai-contest-2026 -m contest2026_252_EPG.xml
repo sync -c -j8
```

manifest 中的 `<linkfile>` 会把 `board/contest_board` 软链到
`vendor/openvela/boards/contest2026_252_board`。

### 2. 编译

在 openvela 工作区根目录（本仓上一级）：

```bash
./build.sh vendor/openvela/boards/contest2026_252_board/configs/nsh --cmake -j8
```

产物：`cmake_out/contest2026_252_board_nsh/nuttx.bin`。

### 3. 生成可烧录镜像

```bash
python3 contest2026_252_EPG/board/contest_board/tools/bk_crc_pack.py \
    cmake_out/contest2026_252_board_nsh/nuttx.bin nuttx_crc.bin
```

### 4. 烧录

```bash
python3 contest2026_252_EPG/board/contest_board/tools/bk_flash.py \
    nuttx_crc.bin 0x11000
```

脚本启动后按一次板子右侧 `RST` 键即可，它会自动完成握手、擦写和回读校验。

之所以自带烧录器而不直接用官方 `bk_loader`：本板的 CH340 控制线没有接到芯片的
CEN 复位脚，官方工具无法自动复位，而它的等待窗口固定约 10 秒，只能靠盲按 RST 去
撞——实测连续 35 轮未命中。本脚本不设超时，任意一次复位都会被捕获。细节见
[board/contest_board/README.md](board/contest_board/README.md) 第八节。

Type-C 线接开发板 `USB TO UART` 口；板载 CH340 接到 UART0（GPIO10/GPIO11），
控制台 **115200 8N1**。

> ⚠️ **务必先备份再烧录。** 烧录会覆盖出厂的声网 AI Demo 固件，而一旦烧入不能
> 启动的镜像，板子将不再提供任何串口响应，只能靠上述持续等待重新接管。备份用
> 官方 `bk_loader read` 或 BKFIL 完整读回 8MB 即可。

## 五、当前进度

| 阶段 | 状态 |
| --- | --- |
| 芯片层（启动/中断/时钟/GPIO/UART/SysTick/堆） | ✅ 完成 |
| 板级层（defconfig / 链接脚本 / 板级初始化） | ✅ 完成 |
| openvela 构建集成 | ✅ 通过，macOS 上可复现 |
| 镜像布局验证 | ✅ 向量表位于镜像首字节，SP/复位地址正确 |
| flash CRC 打包 | ✅ 工具已实现，并用真机读回数据交叉验证 |
| 出厂固件备份 | ✅ 完整 8MB，两次独立读取互证，CRC 块结构零失配 |
| 真机烧录 | ✅ 写入 `0x11000` 成功，回读逐字节一致 |
| 真机启动 | ❌ bootloader 拒绝跳转，原因已定位（见下） |

构建产物已核对：

```
_vectors  0x02010000   ← 向量表在镜像首字节（bootloader 从这里取 SP/PC）
__start   0x02010130
初始 SP   0x28002464   ← _ebss + CONFIG_IDLETHREAD_STACKSIZE，落在 SRAM 内
.data     0x28000000
```

**烧录成功，但未观测到 NuttX 启动；根因尚未定位。**

真机上做了 5 组单变量实验（详见
[board/contest_board/README.md](board/contest_board/README.md) 第九节）。可靠的
结论有两条：

- **bootloader 不校验 app 镜像内容**——把出厂镜像里一个日志字符串的字母翻转
  （1 字节）后照常启动。这**否定了"需要逆向 bootloader、为镜像补 CRC/hash 头"
  的方向**，与官方文档一致；bootloader 中的 `img hash err!` 等字符串属于 OTA 路径。
- **改动复位向量会导致不启动**——只改向量表第 2 个字（4 字节）即可复现。

其余 3 组实验的结论**已作废**：所用探针依赖了未经验证的前提（`SYSRESETREQ` 是否
重启本 SoC、LED 引脚映射靠标号顺序推断、马达所在的 `LDO_3V3` 未先经 GPIO52
使能）。这几次弯路连同排查方法一并记录在板级 README 中——对复现本移植的人，
这比结论本身更有参考价值。

板子状态：出厂固件已从备份恢复并验证可正常启动。

## 六点五、自建 Skill

`.claude/skills/` 下沉淀了四个 Skill，覆盖本次移植中反复要重新踩一遍的流程。
**每一条规矩后面都挂着本仓真实的翻车案例**，PORTING_NOTES 各章可逐条对上，
不是为提交而写的模板。

| Skill | 解决什么 | 代表性教训 |
| --- | --- | --- |
| `hardware-truth` | 取证纪律：读数可信吗、一次一变量、实验要有对照、结论变了先改标题 | 悬空脚被读成"正在充电"并维持三分钟 |
| `flash-firmware` | 构建→CRC 打包→烧录→验证闭环 | 复位窗口只有几十毫秒，进程接缝比它宽；未打包的镜像一定不启动 |
| `serial-verify` | 串口驱动 NSH 做真机验证 | 判"卡死"前先确认有没有观察通道——误判过一次并撤回 |
| `vendor-blob` | 闭源厂商库集成、适配表、符号缺口分析 | `--gc-sections` 吃掉适配表，第一次"构建成功"什么都没链上 |

四个都带触发词，遇到对应场景会自动加载。

## 六、AI Coding 使用说明

本作品全程由 AI（Claude Code）辅助完成，典型协作环节：

- **需求拆解与可行性判断**：先读参赛规范与赛道要求，再对照 openvela 源码树确认
  BK7258 确实未被支持，并找到 `vendor/sifli` 的 SF32LB52 移植作为 out-of-tree
  芯片移植的可行范例，据此确定"公共仓零改动"的技术路线。
- **硬件事实提取**：从 BK7258 Datasheet 与 Beken `bk_idk` SDK 中定位并交叉验证
  寄存器基址、中断号、位域、引脚复用值，避免凭记忆写寄存器。
- **编码**：按 NuttX 的 arch/chip 接口契约实现芯片层，参照同构的 Armv8-M 移植
  对齐代码风格与 API 约定。
- **调试**：编译错误逐轮定位修复（`board` target 重名、NVIC 宏签名不匹配、
  `up_prioritize_irq` 的 Kconfig 依赖、SysTick tick 源缺失、build-id 段挤占
  向量表位置等）。
- **逆向**：厂商 CRC 工具无 macOS 版，通过对已编码样本做参数穷举定出算法并全量
  验证，绕开了平台限制。

完整对话日志见 `logs/` 目录。
