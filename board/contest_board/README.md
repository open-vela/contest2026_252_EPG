# BK7258 板级适配（声网对话式 AI 开发套件 R1）

openvela / NuttX 在 **博通集成 Beken BK7258** 上的板级支持包，目标硬件为
**声网对话式 AI 开发套件 R1（Agora ConvoAI Kit R1）**。

## 一、为什么放在这里

openvela 支持完全 out-of-tree 的芯片 + 板级移植：`CONFIG_ARCH_CHIP_CUSTOM_DIR`
和 `CONFIG_ARCH_BOARD_CUSTOM_DIR` 可以指向 `nuttx/` 之外的任意路径
（`vendor/sifli` 的 SF32LB52 移植就是这么做的）。因此本移植的全部代码都在参赛
仓内，**`nuttx/`、`packages/`、`vendor/` 等公共仓零改动**，符合参赛要求。

manifest 中的 `<linkfile>` 把本目录软链到
`vendor/openvela/boards/contest2026_252_board`，两个 CONFIG 路径都指向那里。

## 二、目录结构

```
board/contest_board/
├── chip/                       # BK7258 芯片层（→ CONFIG_ARCH_CHIP_CUSTOM_DIR）
│   ├── bk7258_memorymap.h      # 内存映射与外设基址
│   ├── bk7258_uart.h           # UART 寄存器定义
│   ├── bk7258_gpio.[ch]        # GPIO / 引脚复用
│   ├── bk7258_clockconfig.[ch] # 外设时钟门控与时钟源选择
│   ├── bk7258_start.c          # 复位入口：VTOR/FPU/.data/.bss → nx_start()
│   ├── bk7258_irq.c            # NVIC 中断控制
│   ├── bk7258_timerisr.c       # 系统时基（arch_alarm，backing 在 AON RTC）
│   ├── bk7258_serial.c         # UART 字符设备驱动
│   ├── bk7258_lowputc.c        # 早期调试输出
│   ├── bk7258_allocateheap.c
│   ├── include/irq.h           # 60 个外设中断号
│   └── Kconfig / CMakeLists.txt / Make.defs
├── src/                        # 板级初始化
├── include/board.h             # 时钟与引脚约定
├── scripts/ld.script           # 链接脚本（含 XIP 地址推导）
├── configs/nsh/defconfig       # 最小 NSH 基线
├── apps/                       # 板级演示应用（经 vendor/openvela/apps 软链进 apps 构建）
│   └── eyes/                   # 双屏机器人眼动画：目光游移 + 眨眼 + 脏矩形增量刷新
└── tools/
    ├── bk_crc_pack.py          # flash CRC 编码 / 校验
    └── bk_flash.py             # 持续等待复位窗口的烧录器
```

## 二点五、apps 接入（本地管道）

演示应用（`eyes`）的代码也在参赛仓内。apps 构建树经 `vendor/` 下两处
**未提交的本地 plumbing** 找到它们（与上面的板级软链同类，公共仓提交
历史零改动）：

```bash
# 1. 软链：apps 构建 → 参赛仓 apps 目录
ln -s ../../contest2026_252_EPG/board/contest_board/apps vendor/openvela/apps

# 2. 桥接文件 vendor/openvela/CMakeLists.txt（仅两行有效语句）:
#      nuttx_add_subdirectory()
#      nuttx_generate_kconfig(MENUDESC "openvela")
```

链路：`apps/vendor -> ../vendor`（仓库自带）→ `vendor/CMakeLists.txt`
glob 一级子目录 → `vendor/openvela/CMakeLists.txt`（桥接）→
`vendor/openvela/apps`（软链）→ 本目录 `apps/`。defconfig 打开
`CONFIG_CONTEST_EYES=y` 后 NSH 内置 `eyes` 命令。

## 三、关键硬件事实（来自 BK7258 Datasheet V2.1 与 Beken bk_idk SDK）

| 项目 | 值 |
| --- | --- |
| CPU | Armv8-M STAR-MC1（Cortex-M33 兼容），最高 480MHz |
| SRAM | 640KB 共享 SRAM @ `0x28000000`（数据视图） |
| Flash | XIP 窗口 @ `0x02000000` |
| PSRAM | `0x60000000` |
| UART0 | `0x44820000`，IRQ 4 |
| UART1 / UART2 | `0x45830000` IRQ 15 / `0x45840000` IRQ 16 |
| 外设中断数 | 60（`INT_ID_MAX`） |
| UART 时钟 | XTAL 26MHz，`baud = 26MHz / (clk_div + 1)` |
| 控制台引脚 | GPIO10 = RX，GPIO11 = TX（同时是 ROM 下载口 DL_UART） |

CPU0 以 Secure（`CONFIG_SPE=1`）运行，故地址偏移为 0，直接使用上述基址。

> 注意：Beken SDK 内部 `GPIO_DEV_UARTn` 的编号比 `UARTn_xx_PIN` 大 1，即硬件
> 文档的 UART0 在 SDK 枚举里叫 UART1。本移植统一采用 **datasheet 的编号**。

## 四、Flash 布局

BK7258 的 flash 控制器在 XIP 取指时做 CRC 校验，**每 32 字节数据后插入 2 字节
CRC**，因此物理地址 = 虚拟地址 × 34/32。由分区表
（`middleware/boards/bk7258/partitions.csv`）：

| 分区 | 物理偏移 | 物理大小 | 虚拟（CPU 可见） |
| --- | --- | --- | --- |
| bootloader | `0x0` | 68KB | — |
| app | `0x11000` | 1836KB | `0x10000`，即 XIP 地址 **`0x02010000`**，1728KB |

链接脚本据此把 `flash` 段起点设为 `0x02010000`。

`tools/bk_crc_pack.py` 实现该 CRC 编码。算法参数（MSB-first、多项式 `0x8005`、
初值 `0xFFFF`、大端存储）是对 Beken SDK 中随附的已编码镜像做参数穷举得到的，
并在 3 个样本共 3868 个块上逐块复算，**零失配**。

## 五、编译

在 openvela 工作区根目录（本仓上一级）：

```bash
./build.sh vendor/openvela/boards/contest2026_252_board/configs/nsh --cmake -j8
```

macOS（Apple Silicon）可用仓库根的封装脚本：

```bash
./build-macos.sh vendor/openvela/boards/contest2026_252_board/configs/nsh
```

产物在 `cmake_out/contest2026_252_board_nsh/`：`nuttx`（ELF）、`nuttx.bin`。

### 两个配置

| 配置 | 用途 | flash 占用 |
| --- | --- | --- |
| `configs/nsh` | 产品镜像：显示管线、摄像头、音频、BLE，外加轻量必测项（`/etc` ROMFS、`md5_test`、BCH、复位原因） | 1652520 B / 93.4% |
| `configs/xts` | 验证镜像：官方 xTS「通用自测用例」全套 + C++（libcxx）；剥掉闭源 BLE 栈与 eyes/face/snap 腾空间 | 1262392 B / 71.3% |

app 分区只有 1728 KB，两者塞进同一个镜像会溢出到 102%。跑必测用 `xts`，
跑完把 `nsh` 烧回去——官方流程本来就是"验证构建 ≠ 出货镜像"。

```bash
./build-macos.sh vendor/openvela/boards/contest2026_252_board/configs/xts
```

> **加了新 CONFIG 就必须先删构建目录**（`rm -rf cmake_out/<board>_<cfg>`）。
> cmake 只在初次配置时把 defconfig 展开成 `.config`，之后的 `olddefconfig`
> 拿的是已有 `.config`，新增行会被静默忽略且构建照样成功。`--cmake distclean`
> 对此无效，它会直接提示让你删目录。

编译后镜像布局（已验证）：

```
_vectors  0x02010000    ← 向量表位于镜像首字节
__start   0x02010130
初始 SP   0x28002464    ← _ebss + CONFIG_IDLETHREAD_STACKSIZE
.data     0x28000000
```

## 六、生成可烧录镜像

```bash
python3 board/contest_board/tools/bk_crc_pack.py \
    cmake_out/contest2026_252_board_nsh/nuttx.bin nuttx_crc.bin
```

`nuttx_crc.bin` 写入 flash **物理偏移 `0x11000`**（app 分区），保持 bootloader
分区不动。

自检：

```bash
python3 board/contest_board/tools/bk_crc_pack.py --verify nuttx_crc.bin
```

## 七、串口

开发套件 R1 板载 CH340（VID `0x1A86` / PID `0x7523`）接到 UART0，
即 GPIO10/GPIO11。控制台参数 **115200 8N1**。

macOS 下设备节点形如 `/dev/cu.usbserial-xxx`。

## 八、烧录：这块板子的实测坑

以下都是在真机上验证过的结论，直接照搬厂商文档会踩坑。

### 8.1 flash 实际型号

bootrom 报告 flash MID `0x1765c8`：厂商 `0xc8`（GigaDevice），容量位 `0x17`
即 2^23 = **8MB**，与 datasheet 中 QFN88 封装的 8MB SiP flash 一致。

### 8.2 `bk_loader` 的自动复位在本板无效

`bk_loader` 的 `--reset_type 0`（DTR/RTS）在本板上**不能复位芯片**，表现为一直卡在

```
Please reset the chip
Waiting reset......
Get bus failed
```

而且 `--retrycnt` 并不会延长这个 10 秒窗口。声网文档对此有过提示——"当烧录工具
无法自动重启开发板时，可以手动重启"。

原因是本板的 CH340 控制线**没有接到芯片的 CEN（复位）脚**，工具无法自动复位；
唯一的复位途径是手按板子右侧的 RST 键。而 `bk_loader` 的等待窗口固定约 10 秒，
`--retrycnt` 也不会延长它——于是烧录变成了"盲按 RST 去撞那 10 秒"的运气游戏。
实测用 for 循环反复启动 `bk_loader` 时，连续 35 轮没命中过。

本目录的 `tools/bk_flash.py` 解决了这个问题：它**不设超时**，持续发
`CMD_LinkCheck` 直到芯片应答，因此**任意一次复位都会被捕获**，不需要抢时机。

```bash
# 烧录（地址为物理偏移，十六进制）
python3 tools/bk_flash.py nuttx_crc.bin 0x11000

# 从备份恢复出厂 app 分区
python3 -c "d=open('bk7258_backup/factory_full.bin','rb').read(); \
    open('/tmp/restore.bin','wb').write(d[0x11000:0x47000])"
python3 tools/bk_flash.py /tmp/restore.bin 0x11000
```

跑起来后按一次 RST 即可，脚本会自动完成握手、擦写和回读校验。

> **务必先备份再烧录。** 烧入不能启动的镜像后，板子不再提供任何串口响应
> （出厂 app 自带的下载响应器随之消失），只能靠上面的持续等待重新接管。本次
> 调试中出现过这种情况，就是用这个脚本恢复的。

### 8.3 bootrom 的链接波特率

一个容易误判的点：在 **115200** 下对 bootrom 发 `CMD_LinkCheck`
（`01 e0 fc 01 00`）会收到看似合法的 4 字节 `04 0e 01 00`，很容易被当成"协议不
兼容的精简帧"。实际在 **1500000** 下收到的是完整帧：

```
04 0e 05 01 e0 fc 01 00
└preamble┘ └─echo──┘ └code=0x01, status=0x00
```

这正是标准 BK72xx BootROM 帧格式，115200 下那个短帧只是波特率不匹配的误读。

因此用社区工具 `bk7231tools` 时，**link_baudrate 必须一并设成 1500000**——它的
CLI 把链接波特率写死为 115200，只能走 API：

```python
from bk7231tools.serial import BK7231Serial
s = BK7231Serial(port="/dev/cu.usbserial-310",
                 baudrate=1500000, link_baudrate=1500000)
s.hw_reset()
s.connect()          # protocol: FULL, flash size: 8 MB
```

### 8.4 CRC 算法的真机交叉验证

从芯片读回的 flash 原始字节是 **CRC 编码后的物理数据**（`bk_loader read` 不做解
码）。用 `tools/bk_crc_pack.py --verify` 校验读回的 bootloader 区，120 个块**零失
配**——这在真实硬件上再次确认了第四节中逆向得到的 CRC 参数。

同时也说明：写入时喂给烧录工具的必须是 **CRC 编码后**的镜像（即 `nuttx_crc.bin`），
地址用**物理**偏移 `0x11000`。

## 九、当前状态

> 控制台之后的外设 bring-up 全史（RX 卡死结案、看门狗复位、灯/键/马达、
> I2C、双屏 QSPI 战役、eyes 动画）见 **[PORTING_NOTES.md](PORTING_NOTES.md)**。
> 本章下方各小节保留为芯片层启动阶段的历史取证记录。

- [x] 芯片层：启动、中断、时钟、GPIO、UART、时基（arch_alarm / AON RTC）、堆
- [x] 板级层：defconfig、链接脚本、板级初始化
- [x] 构建集成：openvela CMake 构建通过，干净重建可复现
- [x] 镜像布局：`_vectors` 位于镜像首字节，SP / 复位地址正确
- [x] flash CRC 打包：对厂商样本与真机读回数据双向验证，零失配
- [x] 出厂固件备份与恢复：完整 8MB，两次独立读取互证，恢复后 Demo 正常启动
- [x] 真机烧录：写入 `0x11000` 成功，回读逐字节一致
- [x] **bootloader 交接**：裸桩在真机执行并输出，交接完全正常
- [x] **`__start()` 全程走通**：9 个检查点全部到达，`nx_start()` 已进入
- [x] 真机控制台：**稳定**（rd_ready 判据 + 监控线程，详见 PORTING_NOTES 一章）
- [x] 看门狗复位与自动重烧（PORTING_NOTES 二章）
- [x] LED / 按键 / 马达（标准 NuttX 驱动，实机验证）
- [x] I2C 主机 ×2 + 位敲备胎（引擎验证,待正向应答从机）
- [x] 双 GC9D01 屏全部走硬件 QSPI（61 字缓冲 RAM 结案,PORTING_NOTES 七章）
- [x] `eyes` 机器人眼动画应用（17fps 实测）
- [x] AON RTC（`/dev/rtc0` + 系统时钟源，计数率实测判定 32000 Hz ROSC）
- [x] 看门狗字符驱动（`/dev/watchdog0`，`wdog` 真机验证会咬人，PORTING_NOTES 十四章）
- [x] PSRAM 16MB（`free` 17.1MB + 五点自检）与 SD NAND（FAT 挂载/写读/重挂持久）
- [x] 摄像头 GC2145：SCCB 验明正身 + DVP/JPEG 取流，`snap` 落盘 640×480 主机解码成功（七点五 / 七点六章）
- [x] 音频：片内 DAC 扬声器 + ADC 麦克风，`face REC` 录放闭环（十二章）
- [x] 原生表情播放管线：SD 读 → 硬件 JPEG 解码 → DMA2D → 双屏 blast，**22.4fps**（十一章）
- [x] 通用 TIMER 驱动（`/dev/oneshot0`，`CONFIG_BK7258_TIMER`，xTS 1.3.13 通过，十九章）
- [x] 片内 flash MTD（`/dev/mtd0` + `/dev/mtdblock0`，`CONFIG_BK7258_FLASH`；仅 `configs/xts` 启用，二十章）
- [x] BLE：闭源控制器上 NuttX，接收与广播均真机验证（十三章）；host 链接崩溃已结案（二十二章）
- [x] WiFi：扫描 / 关联 / DHCP / ping 公网，WPA2 与 WPA3-SAE 端到端（二十一章）
- [x] xTS 必测集：`configs/xts` 上逐项跑过，含 RTC、看门狗、Timer、块设备、RNG（十六 / 十七章）

### 内存布局约束（实测所得）

链接脚本的 RAM 区间不是照抄 SDK 得来的，是量出来的，两条约束方向相反：

**栈必须够高。** 独立探针把 MSP 依次指向 8 个地址并让每个承载真实调用帧：
`0x28032468`、`0x28042468`、`0x28062468`、`0x28082468`、`0x2803c800` 全部通过，
`0x28022468` 直接把 CPU 卡死。所有初始 SP 落在该线以下的镜像都在执行第一条可上报
指令之前就死了。分界线正好落在 bootloader 自己的栈附近（其向量表起始 SP 为
`0x28030000`）——有提示性，但未证实。

**`.data`/`.bss` 必须够低。** 放在 `0x28020000` 时 `__start()` 每个检查点都到；
把它们搬到 `0x28040000`（栈仍用已知可用的地址）则死在 `bk7258_clockconfig()` 里。
**能当栈用和能放 `.bss` 是两个独立性质**，SRAM3 满足前者不满足后者，原因未知。

于是两者都放在 SRAM2：`.data`/`.bss` 从底部起，IDLE 栈顶由
`_ebss + CONFIG_IDLETHREAD_STACKSIZE` 算出，堆用剩下的部分。

> **区间此后扩过两次，上面那两个地址已过时。** 闭源 BLE 库带来 37KB `.bss`
> 时把 IDLE 栈顶顶出了原来的 128K 区界（PORTING_NOTES 十三章第一战），
> 区间先扩到 192K，现为 **256K**：`ld.script` 里 `sram` = `0x28020000`
> 起 256K，`_eram` = **`0x28060000`**，并加了一条链接期 `ASSERT` 让再撞
> 这个坑变成构建错误而不是砖。运行时还用 `kumm_addregion()` 把 SRAM4/5
> 并进堆，42KB → 370KB（第八章）。当前 `configs/nsh` 实测 sram 占用 67.32%。

### 时钟使能的时序依赖

`bk7258_uart_clockenable()` 只做两次读改写，位定义逐位核对过 SDK 全部正确，但曾
稳定地卡在这里。**仅仅在两次写之间插入两个探针调用（唯一效果是消耗约半秒）**，
同一镜像、同一布局、同一配置就一路跑到 `nx_start()`。所以缺的是时间不是数值，代码
里已改为显式的 `bk7258_clock_settle()`。需要多长时间来自探针而非数据手册，故取值
偏保守。

### 已逐项核对无误的部分（对照 bekencorp/bk_idk）

| 项 | 我们的值 | SDK |
| --- | --- | --- |
| UART0/1/2 基址 | `0x44820000` / `0x45830000` / `0x45840000` | 一致 |
| config 偏移与位 | `0x10`；tx_en bit0、rx_en bit1、data bits3-4、clk_div bits8-23 | 一致 |
| fifo_status | `0x18`；`WR_READY` bit20 | 一致 |
| fifo_port | `0x1C` | 一致 |
| global_ctrl 软复位 | `0x08` bit0 | 一致（SDK 只写 1，自清） |
| UART0 时钟使能 | `0x44010030` bit2 | 一致 |
| UART0 分频/选择 | `0x44010020` bits8-9 / bit10 | 一致 |
| UART 时钟频率 | 26 MHz | `CONFIG_XTAL_FREQ=26000000` |
| UART0 引脚与 AF | GPIO10 RX / GPIO11 TX，AF 索引 0 | 一致 |
| 功能模式寄存器 | `0x440100C0 + (n>>3)*4`，位移 `(n&7)*4` | 一致 |

### 本轮修掉的缺陷

1. **`gpio_output_en` 极性写反**（低有效）。`bk7258_gpio_config()` 与
   `bk7258_gpio_setaf()` 共用该位，一个 bug 同时废掉了灯、马达、GPIO11 三种探针，
   使六次实验一致给出假阴性。
2. **堆上界取自芯片常量而非链接脚本**。`up_allocate_heap()` 用
   `BK7258_SRAM_END`（`0x280a0000`，六个 bank 全域）算出 436KB 的堆，跨进实测
   无法承载 `.bss` 的 SRAM3/4/5，而 `kumm_initialize()` 会在堆区两端写管理结构。
   该步骤在 `nx_start()` 中排在串口驱动注册之前 130 行，一挂就永远没有控制台输出。
   已改为链接脚本提供的 `_eram`。
3. **`arm_lowputc()` 无超时**。控制台一旦发不出，整个启动冻结在唯一能报告此事的
   函数里。已改为有界等待。
4. **未复位流控与唤醒寄存器**。SDK 每次 init 都清零 REG_0x0A / REG_0x0B，本移植
   没有，等于继承 ROM 下载器留下的状态。已补上。

### 控制台疑案结案（全部在真机上取证）

从 `nx_start()` 静默到控制台完全可用，先后修掉五个互相叠加的缺陷：

1. **UART 软复位极性**（TX 无输出的元凶）：`global_ctrl` bit0 是持续状态位
   （1=释放），不是自清脉冲。本移植按"进复位、出复位"习惯写 1 再写 0，把发送
   引擎永久按在复位里——FIFO 照收、`WR_READY` 照亮、线上一个字节都没有。
   从出厂 bootloader 反汇编读出"只写 1"后当场解决。
2. **SVCall/HardFault 未挂接**：只设了优先级没挂 handler，首次上下文切换落进
   `irq_unexpected_isr`——横幅打得出来，键入没人处理。
3. **SoC 中断路由矩阵**（RX 无中断的元凶）：BK7258 在 NVIC 之外还有一层按 CPU
   的路由使能（`0x44010080` 起，位号=NVIC 线号），矩阵位不开中断根本到不了
   NVIC。轮询发送不需要中断所以 TX 先好了。已接入 `up_enable_irq()`。
4. **`fifo_status` 的计数/空标志在负载下说谎**（突发丢字节的元凶）：TX 排空
   期间（即每次回显），RX 计数和空标志双向出错——先谎报"非空"让驱动无限弹空
   FIFO（整机楔死），换成按计数守卫后又谎报"空"让已到的字节滞留 FIFO（实测
   `fs=0x003a0000`：`rd_ready=1` 与 `rx_empty=1` 同时成立，数据确在）。唯一
   诚实的是 `rd_ready` 位（TX 侧孪生位 `wr_ready` 全程无故障为旁证）。弹出
   判据全面改用 `rd_ready`，并在 ISR 退出前清扫 `rd_ready` 仍亮的残留字节。
5. **裸 WFI**：厂商固件从不裸执行 WFI（tickless+PM 投票+32K 时基，且默认唤醒
   源被显式关闭）。本移植 idle 暂为空转，睡眠留待按厂商 PM 序列正经实现。

**取证基础设施**（保留在树内，均可用 Kconfig/源码开关控制）：

- `bk7258_wdt.c`：AON 看门狗常备，喂狗跑在 AON RTC 的 oneshot 回调里（时基换成 arch_alarm 后从 SysTick 迁过去的，见 `bk7258_rtc.c:855`）；`board_reset()` 走看门狗
  （SYSRESETREQ 在本 SoC 无效，SDK 与 bootloader 均用看门狗复位——反汇编证实）。
  注意看门狗块**不可在 `__start` 顶端写**（两块都会总线挂死），须在时钟/控制台
  就绪后武装。
- 黑匣子：串口 ISR 向 `_bbnote`（链接脚本给出，紧贴链接区上沿的 128 字节，不在
  任何堆里，看门狗复位不清）写面包屑，重启后回放（`BB=` 行）。地址由链接脚本
  推导而非写死常量——早先写死的 `0x28048000` 在 `LENGTH(sram)` 扩到 192K 后被
  主堆吞掉，ISR 每次中断都往活堆里写 tag，`ls /dev` 顺着被改写的 inode 指针取
  0x3300000c 触发总线错误。
- `conmon` 监视线程：每 3 秒经轮询通道采样中断链全景（UART 使能/状态、NVIC
  使能/挂起/活跃、矩阵位），异常才发声；"黑匣子序号冻结+数据挂起"连续 3 次
  即自动转储并看门狗复位（RX 死人开关）。

### 真机实验记录（第一轮，结论已作废）

下表 3~6 曾被当作"bootloader 不跳转"的证据，**现已证明全部是假阴性**：所有 GPIO
探针都因为 `gpio_output_en` 极性写反而处于高阻态，无论代码是否执行都不可能有信号。
自复位探针则依赖 `SYSRESETREQ`，而该请求在本 SoC 上不触发复位。详见下节复盘。

| # | 改动 | 当时结果 | 现在的解读 |
| --- | --- | --- | --- |
| 1 | 翻转一个日志字符串的字母（1 字节，虚拟偏移 `0x698`） | 正常启动 | 未回读验证，仅旁证 |
| 2 | 只改向量表第 2 个字（4 字节） | 不启动 | 有效：证明 bootloader 确实取用该向量 |
| 3 | 向量改指 `0x200` + 该处放桩 | 桩不执行 | **假阴性**（探针失效） |
| 4 | 向量改指 `0x20000` + 该处放桩 | 桩不执行 | **假阴性** |
| 5 | 向量不动，桩覆盖出厂入口 `0xCAFA0` | 桩不执行 | **假阴性** |
| 6 | 保留前 4KB，其余全填 NOP，末尾放桩 | 桩不执行 | **假阴性** |

### 真机实验记录（第二轮，探针修正后）

探针改用 GPIO11 电平翻转：GPIO11 = UART0 TX → CH340，datasheet 与原理图双重确认。
把它当普通 GPIO 翻转，主机侧收到成帧错误字节流，**收到任何字节即证明代码在执行**。
不依赖时钟门控、引脚复用、UART 外设、供电轨，也不需要肉眼判断。

先做对照：恢复出厂固件后监听同一串口，完整读到其启动日志（`cpu1:(0):driver_init
end`、`cpu0 receive the cpu1 boot success event` 等）。**监听链路本身由此被证明可靠**，
后续"零字节"才具有阴性证据的效力。

| # | 镜像 | 初始 SP | 结果 |
| --- | --- | --- | --- |
| 7 | 40 字节裸桩（无 NuttX 运行时、不碰栈） | `0x2803c800` | **持续输出** |
| 8 | NuttX 探针，复位向量补丁直指 `__start` | `0x280015a4` | 零字节 |
| 9 | 同上，仅改 SP | `0x2803c800` | **106912 字节** |
| 10 | NuttX 全量 + 7 检查点，走完整复位路径 | `0x2803c800` | **7 组脉冲全部到达** |
| 11 | 同上，链接脚本自然产生的 SP | `0x28022464` | 零字节 |
| 12 | 同上，`.bss` 改 8 字节对齐后 | `0x28002468` | 零字节 |
| 13 | 同上，RAM 基址 `0x28020000` + 对齐 | `0x28022468` | 零字节 |

实验 7 是转折点：它排除了 bootloader、CRC、安全态、看门狗等一整类猜想，把问题
限定在本移植的启动代码内。实验 10 进一步证明 `__start()` 从头到尾都能跑完。

**尚未闭合的是初始 SP**：目前只有 `0x2803c800`（出厂固件在用的值）能跑通，而链接
脚本自然产生的三个地址都不行——即使补上 8 字节对齐、即使落在同一个 SRAM bank。
把这三个失败地址与唯一成功地址对比，"必须对齐"和"必须在 SRAM2"两条规则各自都能
解释一部分数据，但都无法解释全部（实验 13 两条都满足，仍然失败）。所以真正起作用的
是 `0x2803c800` 的另一个性质，尚未找到。

### 测量本身的一个教训

实验 10 一度被读成"只有 1 段、死在 VTOR 写入"。那是分段阈值造成的假象：脉冲串之间
的静默约 0.16 秒，而阈值设成了 0.30 秒，七段被糊成一段，再被窗口边界截断成 300~400
字节。改用不做分段假设的原始时间线记录后，7 组清晰可辨。

**推论链上任何一环依赖未经校准的测量，整条链就都不可信**——这已经是本次移植中第二
次因此得出错误结论（第一次是 GPIO 极性）。

### 板级硬件事实（权威来源：原厂固件 Conversational-AI-IOT-Sample + 交叉裁决）

对原厂 demo 仓库（声网 BK7258 beken_genie 工程）做了六路并行源码深读加交叉裁决。
`usr_gpio_cfg.h` 只是上电默认表，与运行时实际用途大量冲突（模板残留），下表以
**运行时代码为准**，每项都核到了消费点：

| 功能 | 引脚 | 依据（运行时代码） |
| --- | --- | --- |
| 红 LED | GPIO40 | `led_blink.h:13`，推挽、高电平亮，整寄存器写 0x2/0x0 |
| 绿 LED | GPIO41 | `led_blink.h:14`，同上；开机默认绿灯常亮 |
| 马达 PWM | GPIO9 | PWM_ID_3 固定映射，1kHz / 占空 30% |
| 外设 3.3V 总开关 | GPIO52 | `CONFIG_LDO3V3_CTRL_GPIO=52`，高有效；LCD/SD/马达/NFC 共享，PM 投票制 |
| 喇叭功放使能 | GPIO50 | `CONFIG_AUD_DAC_PA_CTRL_GPIO=50`，高有效；开 10ms/关 30ms 防 pop |
| 按键 S1 音量+ | GPIO13 | 低有效+内部上拉；长按 5s 配网 |
| 按键 S2 电源 | GPIO12 | 短按 ASR 切换、双击开机、长按 3s 关机；深睡唤醒源（下降沿） |
| 按键 S3 音量- | GPIO8 | 低有效+上拉；低功耗保持输入 |
| 控制台 UART | GPIO10/11 | SDK uart id 0（"UART1"），115200——与本移植一致 |
| I2C1（加速度计） | GPIO0/1 | SC7A20（不是陀螺仪）；GPIO0/1 硬件上**没有** UART1 复用位 |
| I2C0（摄像头 SCCB） | GPIO20/21 | DVP 摄像头配置总线 |
| 模拟 I2C | GPIO42/43 | SCL=42 SDA=43，从设备未定 |
| SDIO（SD NAND） | GPIO14-19 | CLK/CMD/DATA0-3 |
| DVP 摄像头 | GPIO27,29-31,32-39 | JPEG 8bit 接口；电源 GPIO49，reset 疑似 GPIO28（未定论） |
| 双屏 LCD | SPI 接口 | **两块 GC9D01 160x160 SPI 屏**（不是 RGB 并口！RGB 复用是模板残留） |

按键时序（如需在 NuttX 复刻）：6ms 轮询、3 次一致采样消抖、96ms 双击窗口、3s 长按。

**探针史再修正**：LED=GPIO40/41、马达=GPIO9 的原始判断其实都对；那几轮探针没动静
是因为当时镜像死在初始 SP（未对齐/不可用区），根本没执行到探针。马达另需 GPIO52
拉高供电（当时的镜像也做了）——如今启动已修复，这些外设可以直接点亮。

### 三核架构与内存/Flash（原厂配置实测值）

- CPU0 主控 240MHz（上电默认 60MHz，PM 投票拉升）：WiFi/BT/Agora/音频编码，
  SRAM 配额 `0x3D000`；CPU1 多媒体 480MHz：摄像头/双屏/SD/USB，SRAM `0x53000`，
  flash 偏移 `CONFIG_SYS_CPU1_OFFSET=0x22b0000`（= `0x2000000 + 0x2db000*32/34`，
  又一次印证 34/32 CRC 映射）；CPU2：Wanson 离线唤醒词，纯 SRAM `0x10000`。
- CPU1 不是复位自动跑，而是 CPU0 经 `bk_pm_module_vote_boot_cp1_ctrl()` 按电源域
  投票启动——NuttX 后期做 AMP 时对标此模型。
- PSRAM 16MB（APS128XXO_OB9），写穿透模式；厂商把大堆放 PSRAM
  （CPU0 `0x60B00000+2MB`）。**NuttX 单核阶段可整占 640K SRAM，PSRAM 是下一步扩堆方向**。
- 分区表与真机读出的完全一致（bootloader/app/app1/app2/ota + usr_config/easyflash/rf/net）。

### 电源管理旁证（直通本移植的 WFI 疑案）

厂商固件**从不裸执行 WFI**：idle 走 FreeRTOS tickless（`CONFIG_FREERTOS_USE_TICKLESS_IDLE=2`）
+ 32K 时基（`CONFIG_SYSTICK_32K=y`，32K 由 26M 分频而来，无外部晶振）+ PM_V2 投票，
睡前由 PM 驱动完成时钟切换/电压调节/电源域管理。且三个核都显式
`CONFIG_DEFAULT_WAKEUP_SOURCE=n`（SDK 默认唤醒源本是 **GPIO_10 = UART RX**——
说明默认睡眠状态下 UART 本身不叫醒 CPU，要靠 RX 引脚的 GPIO 级唤醒）。

这与"控制台回显 2 字符后永久静默"的现象吻合：NuttX 默认 `up_idle()` 是裸 WFI，
在这颗片子上等于进入一个没有配置唤醒源的睡眠。本移植已改为
`CONFIG_ARCH_IDLE_CUSTOM=y` + 空转 idle（该验证镜像已构建，**尚未上板确认**）。
在按厂商 PM 寄存器序列正经实现睡眠/唤醒之前，idle 不做任何降功耗动作。

### bootloader 启动逻辑（反汇编所得，非推断）

把备份里的 bootloader 分区 CRC 解码后反汇编（`objdump -b binary -m armv8-m.main
-M force-thumb --adjust-vma=0x02000000`），启动路径完全清楚：

`0x02001790` —— 计算 app 地址并跳转：

```
ldr  r0, ="app"          ; 分区名
bl   0x02002194          ; 按名字查分区表
movs r3, #34
ldr  r0, [r0, #52]       ; partition->offset（物理）
lsls r1, r0, #5          ; ×32
sdiv r2, r1, r3          ; ÷34        <- 物理→虚拟换算
add  r0, r2, #0x2000000  ; + XIP 基址
bl   0x0200172c          ; 跳转
```

**这条路径上没有任何校验**：查分区、算地址、直接跳。

`0x0200172c` —— 跳转本体：

```
ldrd r5, r6, [r4]        ; r5 = vector[0] (SP), r6 = vector[1] (PC)
bl   0x02000844
bl   0x02001890          ; 关闭并无效化 cache（见下）
str  r4, [0xE000ED00+8]  ; VTOR = app 基址
msr  MSP, r5
bx   r6                  ; 跳到 vector[1]
```

`0x02001890`（r0=0）操作的是 SCB（`0xE000ED00`）：清 `CCR` 的 bit16（数据 cache
使能），再按 set/way 循环无效化。**即 app 是在 cache 关闭的状态下开始执行的。**

由此得到三条确定结论：

1. **bootloader 不校验 app 镜像**——反汇编实锤，与官方文档一致。
2. **入口机制是标准的**：读向量表取 SP/PC，设 VTOR，跳 `vector[1]`。
3. **app 的 XIP 基址 = `0x02000000 + 物理偏移 × 32 ÷ 34`**。代入本板 app 分区偏移
   `0x11000` 得 **`0x02010000`**——独立验证了 `scripts/ld.script` 里的地址。

第 3 条同时解释了 flash 的 34:32 编码为何贯穿始终：bootloader 自己就在做这个换算。


### 探针设计的复盘

上表中 3~6 的"桩不执行"**都不能作为证据**，各版探针都有缺陷。共同点是**在未经
验证的前提上建结论**：

0. **`GPIO_output_en` 是低有效——所有 GPIO 探针都在高阻态空转。**（已由 bk_idk
   证实，见下）本移植把 bit3 命名为 `GPIO_CFG_OUTPUT_EN` 并置 1 表示"输出使能"，
   而厂商 `gpio_ll_output_enable()` 的实现是 `if (enable) enable = 0;`，注释写明
   *"GPIO output enbale low active"*。置 1 恰恰关掉了输出。灯与马达探针无论代码
   是否执行都不可能有任何动静。这不只是探针的问题：`bk7258_gpio_config()` 与
   UART 引脚复用 `bk7258_gpio_setaf()` 用的是同一个位，**是本移植的真实缺陷**。
1. **依赖 `SYSRESETREQ`——已证伪。** 前两版靠写 `AIRCR` 自复位证明"代码在跑"。
   现已查明整个 Beken SDK **从不调用 `NVIC_SystemReset()`、从不写 `AIRCR`**，其
   `bk_reboot()` 与 bootloader 自身的重启路径（`0x02000fcc`）一律是"把看门狗周期
   设成 6 然后死循环"。SYSRESETREQ 在本 SoC 上显然不触发系统复位，该探针的阴性
   结果不成立。
2. **LED 引脚靠推断。** GPIO40/41 是从原理图网络标号的排列顺序猜的。
3. **负载没有供电。** 马达与 LCD、SD NAND 共用 `LDO_3V3`，其使能脚是 **GPIO52**
   （网络 `LDO33_EN`，引脚 6 = P52）。不先拉高它，马达根本没电。
4. **误认指示灯。** 插 USB 时常亮的绿灯由充电 IC `ETA4322` 驱动，与 MCU 无关。
5. **GPIO9 可能被占用。** datasheet 里 GPIO9 的复用之一是 `32K_XI`，原理图上
   32.768kHz 晶振也画在 P8/P9 附近。
6. **NOP 滑梯撞上 cache 关闭。** bootloader 跳转前会关掉 cache，从 `0xCAFA0` 滑到
   镜像末尾需约 47 万条 NOP，在未开 cache 的 XIP flash 上极慢，几乎必然在到达桩
   之前被看门狗复位。

**修正后的探针**是 GPIO11 位翻转（同样的思路，但极性写对了）：GPIO11 = UART0 TX
→ CH340（datasheet 与原理图双重确认），把它当普通 GPIO 翻转，CH340 会把电平跳变解成
乱码字节，读到任何数据即证明代码在执行。不依赖时钟门控、引脚复用、UART 外设、复位
机制或供电，也不需要肉眼判断。**它在真机上确实输出了**——这就是推翻第一轮全部结论
的那个实验。

早先那一版 GPIO11 探针之所以也没输出，是因为它走的是同一套写反了极性的 GPIO 代码。
一个 bug 同时废掉了灯、马达、GPIO11 三种互相独立的探针，让六次实验一致地给出错误
答案——这正是"多个独立证据都指向同一结论"最危险的情形：它们并不独立。

**写入链路已被独立验证**，不是问题所在：桩所在区域回读后 32 字节逐字节在位，其
虚拟地址算出来正是 `0xCAFA0`（复位向量所指），该区域 240 个 CRC 块零失配；CRC
编解码另做过离线 round-trip，32KB 与 832KB 两个跨度均与原始物理字节逐字节一致。
后来又用完整 8MB 出厂镜像做了第三次验证：120 个已写块的 CRC 与我们的编码器逐块一致。

### 尚未解决（**历史快照：两条都已结案，保留作取证记录**）

> 本节写于控制台尚未通车时。**两条都已解决**：控制台自 PORTING_NOTES 一章
> （`rd_ready` 判据 + 监控线程）起长期稳定；初始 SP 之谜随链接脚本区间扩到
> 256K、并补上链接期 `ASSERT` 一并收口（十三章第一战）。下文保留原样。

`__start()` 已能完整跑完（7 个检查点全部到达），`nx_start()` 已进入，但之后没有任何
控制台输出。同时初始 SP 的规律尚未闭合：只有 `0x2803c800` 能跑通，链接脚本自然产生
的地址即使满足"8 字节对齐 + 落在 SRAM2"也仍然失败。

下一步的两条线：

1. **找出 `0x2803c800` 到底特殊在哪。** 已排除"仅需对齐"和"仅需在 SRAM2"两种解释
   （实验 13 两条都满足仍失败）。可考虑的方向：它接近 SRAM2 顶部而失败的地址都在
   底部；bootloader 自己的栈在 `0x28030000` 向下生长，两者相对位置可能相关。
   最直接的做法是把链接脚本的栈顶钉到 `0x2803c800` 附近先跑通，再回头收窄边界。
2. **`nx_start()` 之后为何无输出。** UART 分频已核对：SDK 用 `UART_CLOCK / baud - 1`
   截断，本移植用四舍五入，115200 下两者都得 224，不构成差异。嫌疑集中在串口驱动
   注册、中断配置，或 `nx_start()` 早期的断言失败。可用同样的 GPIO11 脉冲探针继续
   往 `nx_start()` 内部插检查点。

### 下一步：用 SWD 在跳转处断下（需调试器）

**这一节的前提已经作废，保留仅作参考。** 它写于认为"bootloader 不跳转"的阶段，而
该结论已被实验 7 推翻：裸桩在真机上执行并输出，交接完全正常。若日后仍需硬件断点，
下面的接线与步骤依然有效。

（以下为原文）黑盒探针已经走到尽头——bootloader 的行为完全查清且与本移植吻合，但合规镜像仍不
执行。要再进一步，最有效的是在跳转指令处断下，直接观察跳转后 PC 落在哪、是否触发
fault。

**接线**（引脚来自本板原理图 Sheet 2 与 BK7258 datasheet）：

| 信号 | BK7258 引脚 | 说明 |
| --- | --- | --- |
| SWCLK | 83（`P20/0SCL/SWCLK/R6/D9`） | 板上复用为 `IIC1_SCL`，接 G-Sensor |
| SWDIO | 84（`P21/0SDA/SWDIO/ADC6/R5/D8`） | 板上复用为 `IIC1_SDA` |
| GND | 任意地 | — |

注意这两脚在本板上被 G-Sensor 的 I2C 占用，需确认有测试点或焊盘可接；引脚 43 另有
独立的 `SWD` 信号，用途需查 datasheet 确认。

**软件**：`brew install open-ocd`。BK7258 是 Armv8-M（STAR-MC1，Cortex-M33 兼容），
可先用通用 Cortex-M 配置起步：

```bash
openocd -f interface/cmsis-dap.cfg -c "transport select swd" \
        -f target/swj-dp.tcl -c "adapter speed 1000" \
        -c "swj_newdap bk7258 cpu -irlen 4; dap create bk7258.dap -chain-position bk7258.cpu" \
        -c "target create bk7258.cpu cortex_m -dap bk7258.dap" -c init
```

**断点位置**（地址来自本目录记录的反汇编）：

| 地址 | 含义 |
| --- | --- |
| `0x02001790` | 计算 app 地址的函数入口 |
| `0x0200172c` | 跳转函数入口，`r0` = app 基址（应为 `0x02010000`） |
| `0x02001738` | 刚读完 `vector[0]/[1]`，此时 `r5`=SP、`r6`=PC |
| **`0x02001782`** | **`bx r9`——跳转本身，在此单步进入即可看到目标处第一条指令** |

要确认的三件事：

1. `0x0200172c` 处 `r0` 是否等于 `0x02010000`（若不是，说明分区表解析与预期不符）
2. `0x02001738` 之后 `r5`/`r6` 是否等于我们镜像的 `vector[0]`/`vector[1]`
3. 在 `0x02001782` 单步后 PC 落在哪——若立即进入 HardFault/BusFault 处理，读
   `CFSR`(`0xE000ED28`)、`HFSR`(`0xE000ED2C`)、`BFAR`(`0xE000ED38`) 即可定位故障源

若 SWD 连不上，先确认 eFuse 是否禁用了调试口（本板出厂固件是量产固件，有此可能）。
