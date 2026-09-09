<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **contest2026_252_EPG** (2014 symbols, 2848 relationships, 91 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> Index stale? Run `node .gitnexus/run.cjs analyze` from the project root — it auto-selects an available runner. No `.gitnexus/run.cjs` yet? `npx gitnexus analyze` (npm 11 crash → `npm i -g gitnexus`; #1939).

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows. For regression review, compare against the default branch: `detect_changes({scope: "compare", base_ref: "dev-ai-contest-2026"})`.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `query({search_query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `context({name: "symbolName"})`.
- For security review, `explain({target: "fileOrSymbol"})` lists taint findings (source→sink flows; needs `analyze --pdg`).

## Never Do

- NEVER edit a function, class, or method without first running `impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `rename` which understands the call graph.
- NEVER commit changes without running `detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/contest2026_252_EPG/context` | Codebase overview, check index freshness |
| `gitnexus://repo/contest2026_252_EPG/clusters` | All functional areas |
| `gitnexus://repo/contest2026_252_EPG/processes` | All execution flows |
| `gitnexus://repo/contest2026_252_EPG/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->

---

# openvela 驱动开发与平台适配（本仓落地版）

本仓是 **BK7258（Beken，Armv8-M / STAR-MC1 三核 AMP）板级适配**，目标硬件为声网对话式 AI 开发套件 R1。
下面是官方流程 + 本仓的实际映射，动代码前先对照这里。

权威来源：
- 官方 [新平台适配指南](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/chip_porting/porting_guide.md)（本地：`../docs/zh-cn/chip_porting/porting_guide.md`）
- 官方 [驱动开发](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/device_dev_guide/driver/driver_development.md)（本地：`../docs/zh-cn/device_dev_guide/driver/driver_development.md`）
- 本仓 [board/contest_board/README.md](board/contest_board/README.md) —— 硬件事实、flash 布局、编译烧录、当前状态
- 本仓 [board/contest_board/PORTING_NOTES.md](board/contest_board/PORTING_NOTES.md) —— 逐个外设的 bring-up 实录与真机取证

## 一、三层架构 → 本仓映射

openvela 分**架构层（Architecture）/ 芯片层（Chip/SoC）/ 板级层（Board）**三层。架构层（armv8-m）已支持，不动。

| 层 | 官方标准位置 | 本仓位置 |
|---|---|---|
| 芯片层 | `vendor/<vendor>/chips/<chip>/` | `board/contest_board/chip/` |
| 板级层 | `vendor/<vendor>/boards/<chip>/<board>/` | `board/contest_board/{src,include,scripts,configs}/` |
| 板级驱动 | `boards/<arch>/<chip>/<board>/src/` | `board/contest_board/src/` |
| 通用驱动 | `nuttx/drivers/` | **不要动** —— 公共仓零改动是参赛硬约束 |

芯片层被塞进 board 目录内，是为了让公共仓一行不改。挂载靠 `configs/nsh/defconfig`：

```
CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/openvela/boards/contest2026_252_board/chip"
CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/openvela/boards/contest2026_252_board"
```

`vendor/openvela/boards/contest2026_252_board` 是 repo manifest 的 `<linkfile>` 软链，指回本仓 `board/contest_board`。
**改路径要同步 `contest2026_252_EPG.xml`。**

## 二、驱动模型：和 Linux 不一样的四件事

1. **无 match / probe**：没有 bus-device-driver 匹配，也没有探测。
2. **无 major/minor**，没有设备号概念。
3. **无 `module_init`** —— 驱动不会自己起来，**必须在板级代码里显式调用注册函数**。这是最容易漏的一条。
4. **注册即建节点**：`register_driver(path, fops, mode, priv)`（块设备 `register_blockdriver`）把 inode 挂进伪根文件系统，`/dev/xxx` 存在就代表设备已就绪。

**上下半部分工**：上半部（`nuttx/drivers/`，openvela 提供）实现 `file_operations`，对接系统调用；下半部（我们写）操作寄存器、处理中断，在中断里回调上半部。板级代码负责把两半绑起来并注册。

`boardctl` 是非标准接口（`board_app_init`、`board_poweroff`、`board_reset` 等）；应用侧优先用字符设备 `ioctl`，别直接调 `boardctl`。

## 三、板级四阶段初始化（注册驱动放对地方）

| 阶段 | 执行上下文 | 放什么 | 本仓实现 |
|---|---|---|---|
| `board_early_initialize` | idle 任务之前 | 早期硬件（时钟、关键引脚） | [src/bk7258_boardinit.c:55](board/contest_board/src/bk7258_boardinit.c#L55) |
| `board_late_initialize` | Appbringup 线程 | 常规驱动注册 | [src/bk7258_boardinit.c:75](board/contest_board/src/bk7258_boardinit.c#L75) |
| `board_app_initialize` | nsh 任务 | 文件系统、核心服务 | [src/bk7258_appinit.c:67](board/contest_board/src/bk7258_appinit.c#L67) |
| `board_app_finalinitialize` | nsh 任务 | 应用相关收尾 | **未实现**（需 `CONFIG_BOARDCTL_FINALINIT`） |

## 四、芯片层必须实现的接口（附本仓落点）

| 接口 | 职责 | 本仓文件 |
|---|---|---|
| `__start` | 清 .bss、拷 .data/RAM 函数、初始化时钟与串口、设栈限 → `nx_start()` | `chip/bk7258_start.c` |
| `arm_earlyserialinit` / `arm_lowputc` | `nx_start` 之前的控制台寄存器初始化与打印 | `chip/bk7258_serial.c`、`chip/bk7258_lowputc.c` |
| `up_putc` | OS 内部日志出口 | `chip/bk7258_serial.c` |
| `arm_serialinit` | `uart_register("/dev/console", ...)` | `chip/bk7258_serial.c` |
| `up_timer_initialize` | 系统节拍 | `chip/bk7258_timerisr.c`（SysTick） |
| 中断 `up_*` + `irq_attach` | 使能/屏蔽/优先级 | `chip/bk7258_irq.c` |
| `up_allocate_heap` | 起始 = `ebss + CONFIG_IDLETHREAD_STACKSIZE`，大小 = RAM 末 − 起始 | `chip/bk7258_allocateheap.c` |

`chip.h` / `irq.h` 有**局部**与**公共**两份：局部放当前目录给芯片代码用；公共放 `chip/include/`，架构层通过 `<arch/chip/chip.h>`、`<arch/irq.h>` 引。

链接脚本 `scripts/ld.script`：`EXTERN(_vectors)` 必须保留（向量表要在镜像首字节）；要 backtrace 得加 `.arm.exidx` 段。

## 五、加一个新驱动的落地顺序

1. 判断归属：芯片内置外设 → `chip/`；板上外接器件 → `src/`。
2. 找 openvela 有没有现成上半部（`nuttx/drivers/` 下的 `*_register()` / `struct xxx_ops_s`）。**有就只写下半部**，别自己造字符设备。
3. 写下半部：填 ops 结构体 / `file_operations`，寄存器操作与中断处理。
4. `Kconfig` 加开关，`CMakeLists.txt`（和 `Make.defs`，如该目录有）加源文件。
5. **在 `board_late_initialize` 或 `board_app_initialize` 里显式调用注册**（见第二节第 3 条）。
6. `configs/nsh/defconfig` 打开对应 CONFIG。
7. 编译 → 打包 → 烧录 → **真机验证**，然后把结论补进 PORTING_NOTES.md。

## 六、构建 / 打包 / 烧录

在 openvela 工作区根目录（本仓上一级）：

```bash
./build.sh vendor/openvela/boards/contest2026_252_board/configs/nsh --cmake -j8
```

macOS（Apple Silicon）用仓库根封装：`./build-macos.sh vendor/openvela/boards/contest2026_252_board/configs/nsh`

产物在 `cmake_out/contest2026_252_board_nsh/`（`nuttx` ELF + `nuttx.bin`）。BK7258 的 flash 控制器 XIP 取指时做 CRC 校验（每 32 字节插 2 字节 CRC），**烧录前必须打包**：

```bash
python3 board/contest_board/tools/bk_crc_pack.py cmake_out/contest2026_252_board_nsh/nuttx.bin nuttx_crc.bin
```

写入 flash 物理偏移 `0x11000`（app 分区，别碰 bootloader 分区）。控制台 UART0 / GPIO10-11 / CH340 / 115200 8N1。
烧录的实测坑（复位窗口、波特率、flash 型号）见 board README 第八章 —— **动烧录前先读那一章**。

## 七、与官方指南的已知差距

1. **定时器用的是 `arch_timer`（SysTick），不是官方优先推荐的 `arch_alarm`。** 全仓无 `up_alarm_set_lowerhalf` / oneshot lower-half，tickless 未开。官方理由是 oneshot 模型免去重装计数器的累计误差、精度更高。
2. **`board_app_finalinitialize` 未实现**（`CONFIG_BOARDCTL_FINALINIT` 未开）。目前没有需要它的场景，新增应用级收尾初始化时再补。
3. **xTS 精简集未跑。** 官方把「通用自测用例」列为**必测**（内存、调度、GPIO、I2C/SPI、UART、RTC、Watchdog）。本仓用的是自建真机验证矩阵（PORTING_NOTES 第十章）。其中 RTC 与 Watchdog 已补齐并真机验证（`/dev/rtc0` + `/dev/watchdog0`，见 PORTING_NOTES 十四章），但**跑的是自建用例，不是 xTS 精简集本身**。

   RTC 有两条限制要知道：计数器不跨复位，墙钟时间掉电或重启即丢（`havesettime()` 如实返回 false）；AON 计数率是启动时实测判定的（这块板子是 32000 Hz 内部 ROSC，不是 32768 晶振），改动 `bk7258_rtc.c` 时别把它换成编译期常量。

## 八、纪律

- **公共仓零改动**：`nuttx/`、`packages/`、`vendor/` 一行不能改，全部改动落在本仓。
- **硬件事实要有出处**：寄存器、中断号、引脚复用值必须标明来自 Datasheet 或 `bk_idk` SDK，不靠推断。
- **画面/行为只认真机**：编译通过不等于跑通，结论以真机取证为准。
