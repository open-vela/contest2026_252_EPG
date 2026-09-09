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
| `arm_lowputc` | `nx_start` 之前的控制台初始化与打印 | `chip/bk7258_lowputc.c` |
| ~~`arm_earlyserialinit`~~ | **定义了但从没被调用**——早期控制台走 `bk7258_lowputc()`，`__start` 不调它，NuttX arch 侧也没有调用点。它里面那句 `isconsole = true` 因此从未执行，Ctrl-C 曾经整条链失效就是这么来的（PORTING_NOTES 十七章）。动它之前先读那一节。 | `chip/bk7258_serial.c` |
| `up_putc` | OS 内部日志出口 | `chip/bk7258_serial.c` |
| `arm_serialinit` | `uart_register("/dev/console", ...)` | `chip/bk7258_serial.c` |
| `up_timer_initialize` | 系统时基 | `chip/bk7258_timerisr.c`（arch_alarm，oneshot backing 在 AON RTC） |
| 中断 `up_*` + `irq_attach` | 使能/屏蔽/优先级，含 NVIC 之前那道 SoC 路由矩阵 | `chip/bk7258_irq.c` |
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

1. ~~**定时器用的是 `arch_timer`（SysTick）**~~ —— **此条已过时**。时基早已换成官方推荐的 `arch_alarm`：AON RTC 的 TICK 比较单元backing oneshot lower-half，`chip/bk7258_timerisr.c:146` 调 `up_alarm_set_lowerhalf()`，`.config` 里 `CONFIG_ALARM_ARCH=y` / `CONFIG_ONESHOT=y` 而 `CONFIG_ARMV8M_SYSTICK` 未设（见 PORTING_NOTES 十四章）。**仍然为真的部分**：tickless 未开（`CONFIG_SCHED_TICKLESS` is not set）。

   ~~另有一条**新差距**：`/dev/oneshot0` 未注册~~ —— **此条已过时**。AON RTC 只有两个硬件比较单元（`TICK` 给系统时基、`UPPER` 给 `/dev/rtc0`），第三路已另起片内通用 TIMER 外设：`CONFIG_BK7258_TIMER=y`，注册在 [src/bk7258_appinit.c:188](board/contest_board/src/bk7258_appinit.c#L188)，1.3.13 真机通过（PORTING_NOTES 十九章）。
2. **`board_app_finalinitialize` 未实现**（`CONFIG_BOARDCTL_FINALINIT` 未开）。目前没有需要它的场景，新增应用级收尾初始化时再补。
3. **xTS 精简集：首轮上板 14 项通过、3 项缺陷、3 项跑不了；此后 1.3.13 Timer、1.3.15 看门狗、1.3.5 块设备与片内 flash、1.3.16 RNG 四项已逐一补齐并真机通过（详见本条下方各段）。** 官方把「通用自测用例」列为**必测**。配置层面按清单逐条补齐见 PORTING_NOTES 十六章，**真机执行记录见十七章**。

   **通过**：内存、调度、ostest、getprime、mm、scanftest、helloxx、popen、pipe、md5、cxxtest、fstest、ramtest、RTC（3/3）、crypto（8/8）。

   **`CONFIG_NAME_MAX` 必须 >32**（本仓设 64）。xTS 的 syscall 用例拿 `__func__` 拼文件名，最长 35 字符；NuttX 默认 32 会让 `open()` **静默失败**，用例报 `fd > 0` / `open test file fail`，看着像文件系统坏了。1.1.3 曾因此 8 项失败，改完只剩 2 项（socket，需 TCP/IP 栈）。

   **1.3.15 看门狗已修复并全部通过**（十八章）。接上了 BK7258 的 NMI 看门狗阶段：`0x44800000` 那块抬 NMI 异常、比 AON 块先咬，ISR 里记录 `RESET_SOURCE_WATCHDOG` 再 panic，AON 块随后复位。`cmocka_driver_watchdog -r 3` 四子测试全 PASSED，含 `WDIOC_CAPTURE`。开关是 `CONFIG_BK7258_WDT_NMI`，关掉即回到旧行为。

   **两条纪律**：NMI 块在外设域，`0x44800000` 未上电时访问会挂总线，初始化顺序（先开 `0x44010030` bit31 时钟、再旁路门控、最后才写周期）不能乱；周期单位是 2 kHz，来自厂商 `CONFIG_INT_WDT_PERIOD_MS=8000` 与 `wdt_ll_set_period()` 的 ×2 换算，改周期前先看十八章。

   **1.3.5 块设备在真卡上已跑通**（`cmocka_driver_block -m /dev/mmcsd0`，3/3 PASSED，耗时约 2 小时）。**⚠️ 此前报过的"块设备压测打死板子"是误判，已撤回**（详见 PORTING_NOTES 十七章缺陷二）。`cmocka_driver_block` 是 NSH **前台任务**，运行期间 NSH 本来就不回显，而该测试循环里一个字也不打印——"发命令没回显"被误当成"系统死了"。后台重跑 889 秒，`ps` 显示任务始终 `Ready`（不是 `Waiting`），NSH 全程响应，板子没重启；前台被动重跑 601 秒同样正常。这个测试在 120 MB 卡上要跑 233472 次迭代，**推算数小时**，等 300 秒就下结论是不够的。

   **1.3.16 RNG 的两道坎，都已查清**：
   - **打包**：`apps/testing/drivers/nist-sts` 的解压目录名与自身 CMakeLists 的 glob 不符，且 `PATCH_COMMAND` 的 `-d` 深了一级。修法见 `board/contest_board/tools/fix_nist_sts.sh`，全部动作在 gitignored 下载目录内，**不动公共仓**。fresh checkout 后要重跑一次。
   - **流上限**：`_POSIX_STREAM_MAX` 在 `nuttx/include/limits.h:131` 硬编码 16 且**无 Kconfig**，每任务最多 13 个可用 FILE 流（`fdtest` 实测：裸 `open()` 到 40 无碍，`fopen()` 卡在 13）。NIST 套件要 30+，**全量跑不可能**。这不是板级问题，值得上游报。
   - **绕法**：测试选择那步答 `0` 不全选，用 15 位位串每轮选 ≤5 个测试，分三批跑。**15 项全部产出结果且全部达标**（含 NonOverlappingTemplate 的 148 个子项与 RandomExcursions(+Variant) 的 26 行，零个不达标标记）。
   - **每次重烧镜像后**必须重建 `/tmp/experiments/AlgorithmTesting/<15 个测试名>`——tmpfs 会被清空，否则报 "Could not open freq file"，看着像别的问题。

   **纪律**：判"卡死"之前先确认有没有观察通道。长任务丢后台（`&`）留出 shell，`ps` 一眼分清 `Ready`（在跑）和 `Waiting`（真卡住）。另外 DTR/RTS 无响应不能当死机证据——本板 CEN 没接 CH340 控制线，它**永远**无效（README 8.2）。

   **控制台 Ctrl-C 可用**（`CONFIG_TTY_SIGINT` + `CONFIG_SIG_DEFAULT`，外加 `arm_serialinit()` 里一行 `CONSOLE_DEV.tc_lflag |= ISIG`）。根因是本移植从不调用 `arm_earlyserialinit()`，`isconsole` 在注册时为假，`uart_register()` 那条设 `ISIG|ECHO|ICANON` 的语句从没执行——**不要改成设 `isconsole`**，那会连带打开驱动层回显与规范模式，和 NSH 的 readline 打架。诊断工具 `sigtest` 留在树里（`tcgetattr` 读 `c_lflag`、`TIOCSCTTY` 返回值判 pid 槽）。实测 `ostest` 8.5 秒被杀掉。详见 PORTING_NOTES 十七章。
   - ⚠️ **该压测是破坏性的**：`SECTORS_RANGE 0.95`，对整卡 95% 扇区写随机数据。它擦掉过一次原厂表情素材。跑任何带 stress 的用例前先读源码、先备份目标盘。

   **两个配置，别混用**：
   - `configs/nsh` —— 产品镜像，含轻量必测项（`/etc` ROMFS、`md5_test`、`BCH`、复位原因、`/dev/oneshot0`）。flash 45.10%。
   - `configs/xts` —— 验证镜像，全套必测 + C++（libcxx），剥掉闭源 BLE 栈和 eyes/face/snap 三个演示 app。flash **60.40%**（38.93% 是 2026-08-18 加入 wpa_supplicant + mbedtls 之前的数字）。跑完必测把 `nsh` 烧回去。

   **app 分区已扩到 3648 KB**（原 1728 KB），所以"装不下"不再是常态约束——两个配置现在都有一倍以上余量。扩的依据见 `scripts/ld.script` 顶部：bootloader 按名字查分区、只取 offset、**不读 size 也不校验镜像**（README 第九章反汇编实锤），所以镜像可以长过 app 分区；长过去覆盖的是 app1/app2（CPU1/CPU2 镜像，而 `start_cpu1_core()` 由 CPU0 应用代码调用、我们从不调，那两个核从未启动）和 download（OTA 暂存，本仓不做 OTA）。上限是 `usr_config`（0x3DA000），它和其上的 `rf_firmware`/`net_param`（出厂 RF 校准，BLE 依赖）**必须保留**。

   已真机验证：造一个 2060 KB 的镜像（越过旧边界约 330 KB，打包后确实写进 app1 区域），板子正常启动、越界处的数据 XIP 读回正确、`crc_err_num=0`。注意**只造一个大镜像烧进去不算验证**——第一次尝试的填充数组被 `--gc-sections` 回收了，镜像根本没超限。

   厂商分区表累加正好 4096 KB，是按 4 MB 型号画的，而本板是 8 MB：**上半部 4 MB 未分配**。它不能用来扩可执行镜像（bootloader 把 app 当作从 0x11000 起的一整块连续镜像），只能做数据——已给片内 flash MTD 用（`0x500000..0x7F0000`）。若要放只读大资源（模型权重、字体）可直接 XIP 寻址、不占 RAM，但要写 **CRC 编码后**的字节，且那片区域就不能再当原始 MTD 用（`crc_en` 是全局一位）。

   **唯一不能一次跑完的一项**：1.3.16 RNG —— 15 项全部达标，但受 `_POSIX_STREAM_MAX` 硬编码所限**只能分三批**跑（原因见 PORTING_NOTES 十六章末）。

   **1.3.5 片内 flash MTD 已补齐并真机通过**（`chip/bk7258_flash.c` → `/dev/mtd0` + `/dev/mtdblock0`，`CONFIG_BK7258_FLASH`，详见 PORTING_NOTES 二十章）。区间 `0x500000..0x7F0000`（2.94 MB），是实测挑出来的：厂商分区表累加正好占满 `0x400000`，而 8MB 芯片**最后 6 个扇区在用**，`0x7fe000` 开头是 `"TLV"` 出厂校准数据（BLE 很可能靠它），所以上边界留了整 64 KB。驱动对每一条擦写路径做区间校验，越界直接 `-EFAULT`。

   三个会静默失败的点：① 控制器在 `0x44030000`（`dev_id` 读出 `"FLSH"`），厂商树里另一套被 `CONFIG_SOC_BK7256XX` 包着、指向 `0x00803000` 的定义是陷阱；② 软件通路是**物理地址原始字节**，CRC 只在 XIP 取指通路上，MTD 完全不涉及；③ 芯片**出厂全片写保护**（BP=`0x1f`），不解保护写入会被 flash 静默丢弃、读回来像驱动是死的。诊断工具 `flashtest` 留在树里。产品镜像 `configs/nsh` **未启用**（对 demo 无用途）。~~"已 95.16%"~~ 那个理由已过时：95% 是 app 分区扩容前按 1728 KB 算的，扩到 3648 KB 后同一镜像是 **45.10%**。

   ⚠️ **补它的过程撞出一个时基缺陷，已修，影响所有配置**：`bk7258_rtc_arm()` 对已过期目标退到「最小 1 tick」= 31 µs，而 AON 在常开域、从 CPU 侧写比较寄存器**本身就要最多 31 µs 才生效**——比较值可能在写入落地前被计数器越过，比较器靠**相等**匹配，于是永不匹配、时基永久死亡。现象是**板子答得动却没有时钟**(UART 走自己的中断，所以控制台照常回显)，`sleep` 永不返回，喂狗随之停止，恰好一个看门狗周期后咬。修法是把余量做进值里(最小 4 tick)，**不是**写完回读验证——试过，无效，因为回读那一刻新值还在路上。同一条跨时钟域规则本仓已踩第二次(十九章 TIMER 的 W1C 位)。

   ⚠️ **NMI 看门狗周期一直算错 8 倍，已修**:计数率被硬编码成 2 kHz,实测 `clkdiv=0` → **16 kHz**——那个自称 8 秒的看门狗**从接上那天起一直是 1 秒的**。厂商 `wdt_ll_set_period()` 从来不假设，每次从硬件读 `ckdiv_wdt`。另有超限用掩码截断而非钳位(128000 被截成 62464)，把「要求过长」变成「实际很短」。启动日志现在会打 `wdt: nmi clkdiv=... period ... = ... ms`,让这件事每次启动都可见。

   **1.3.13 Timer 已补齐并真机通过**（`chip/bk7258_timer.c` → `/dev/oneshot0`，`CONFIG_BK7258_TIMER`，详见 PORTING_NOTES 十九章）。用片内 TIMER 组 0，**占两个通道**：ch0 按需装填做 oneshot，ch1 满量程自由跑做 `ONESHOT_CURRENT` 的时基——用例拿前后两次 current 之差核对延时，所以 current 必须单调自由运行，一个通道办不到。系统 tick 仍走 AON RTC，两者互不干扰（oneshot 与 rtc 用例已同轮验证）。

   这块的三个坑都会**静默失败**，动它之前先看文件头注释：① 计数时钟要靠写 `global_ctrl` 的 `soft_reset` 才启动，只开系统控制器那边的门控不够——不写这一位，通道使能了、终值装了，读回恒零且握手永不完成，而 `dev_id` 照样读出 `"TIMR"`、寄存器照样存得住值；② 没有独立中断使能位，`timerN_int_en` 是读回即状态、写 1 清除；③ 清中断必须自旋到读回为 0（跨 26 MHz 时钟域），否则处理函数立即重入。诊断工具 `timertest` 留在树里（读三个时钟位 + 握手是否超时 + 计数增量，四个实验一次跑完）。

   **WiFi：已端到端跑通**——扫描、关联、DHCP、ping 公网，WPA2 与 WPA3-SAE 均真机验证（详见 PORTING_NOTES 二十一章）。~~"链入并跑到 `mm_init`"~~ 是本条最初的标题，早已过时。

   ⚠️ **此前"卡在 `sm_task.h` / `ps.h` 两个没发布头文件"的结论是错的,已撤回。** 那两个 `#include` 都在 `#if NX_VERSION > NX_VERSION_PACK(6,22,0,0)` 里,而本树是 6.8.2.0,预处理器根本走不到。错因是 grep 到 include 行就下结论、没看它被什么守着。`components/bk_wifi/src` 的 **33 个源文件按发布原样全部编过**,不需要向厂商索要任何东西。同批被推翻的还有"`bk_idk` 是声网定制裁剪版"(`git remote` 是 `bekencorp/bk_idk`,两份 SDK 都是官方版)。

   **已上板验证:扫描跑通,`configs/xts` 实测搜到 22 个 AP**(`scanu_confirm: upload_cnt=49, recv_cnt=49, result=22`)。此前"跑到 `mm_init` 硬故障、尚不能收发"的记录已过时。让接收从零变成有,靠的是两条,**两条都是本移植自己的缺陷,不是厂商库的问题**:

   - **`ate_is_enabled()` 答 false 会把 MAC 停进 doze。** `rwnx_intf_init` 在 `wifi_init()` 末尾的 `!ate_is_enabled()` 分支里调 `rwnxl_sleep()` + `ps_env_set_ps_on(true)`,而本移植在 WiFi 核心线程之外没有唤醒机制。厂商的射频工具都在 ATE 模式下跑,所以从不碰到。开关是 `CONFIG_BK7258_WIFI_ATE`(**default n**,只在 `configs/xts` 打开)+ 运行时 `bk7258_wifi_ate_enable()`,必须在 `bk7258_wifi_ifup()` 之前设。四个调用点都读过,ATE=true 只会让协议栈更宽松,**不存在**"需要外接测试仪器"那回事(旧注释是猜的,已撤)。
   - **扫描请求的通配 BSSID 是广播,不是全零。** `bk7258_wifi_scan_start()` 原来把 `SCAN_PARAM_T` 整个清零,注释还写着"全零=任意 BSSID"。`rw_msg_send_scanu_req()` 原样透传(`rw_msg_tx.c:1046`),`scanu_frame_handler` 拿它逐字节比对每一帧、**先计数再丢弃**,于是 `recv_cnt=39 / upload_cnt=0`。厂商 supplicant 驱动写得很明白:`params->bssid ? params->bssid : broadcast_ether_addr`(`driver_beken.c:1902`)。

   **诊断纪律(这一段是花了好几轮才换来的)**:电源域、时钟门控、RC 状态位、RX 描述符环、MPIF —— 全部量过,**全部本来就是对的**。真正点破问题的是厂商自己的断言 `MAC is in doze, open maccore and phy clock`。`src/bkreg.c` 留在树里,预设一条命令解码那四个关键寄存器并把极性直接写成 on/OFF(`0x44010040` 是掉电位,已经被读反过一次)。

   **`wapi scan_results wlan0` 也已跑通**,标准 NuttX 无线 API 直出 BSSID / 频率 / 信号 / SSID(中文 SSID 正常)。这一段又是两个本移植自己的缺陷:

   - **`sr_get_scan_results()` / `sr_release_scan_results()` 不是锁,是引用计数**,而且结果集诞生时 `ref == 0`(`rw_msg_rx.c:1330`),设计上活到**下一次扫描**或连接才被冲掉(`:1322`)。所以读取方只要成对 get/release,就会把 `ref` 从 0 抬到 1 再落回 0,`sr_free_all()` 当场把结果全释放。放每项外面、放整个遍历外面都一样错(两版都试过)。**只读就别碰计数器**——直接读全局 `scan_rst_set_ptr`,厂商自己的 `sr_get_scan_number()` 就是这么干的。
   - **SSID 必须内联在缓冲区里**,`u.essid.pointer` 存的是**偏移**不是地址:`wapi_event_stream_extract()` 按 `current + offsetof(iw_event,u) + pointer` 还原(`apps/wireless/wapi/src/wireless.c:296`)。原来存的是栈上局部变量地址。照抄公共仓 `bcmf_driver.c:1065` 的写法即可,`len = IW_EV_LEN(essid) + 4字节对齐(ssid_len)`。

   另外 `wapi_scan_stat()` 会先用**一个字节**的缓冲区探一次,靠 `-E2BIG` / `-EAGAIN` 区分"有结果"和"还没好";返回 `OK` + 截断流会让它以为一个字节就够了。

   **网络已端到端跑通(2026-08-18 真机)**:`wapi scan` → `wapi essid`(开放 AP,carrier 随真实关联状态抬起)→ `ifconfig wlan0 dhcp`(拿到地址)→ `ping` 网关 5/5、**ping 公网 223.5.5.5 3/3**。数据通路两个方向都是拷贝式:TX 经 `bk7258_wifi_tx_alloc()`(`PBUF_RAW_TX`,96 字节 MSDU 头部空间,`rwnx_start_xmit` 就地包 sk_buff)→ `bmsg_tx_sender(p, vif)`;RX 在 `bk7258_wifi_rx_frame()`(厂商核心线程,任务上下文)`netpkt_alloc/copyin` 进环,`receive()` 弹出。**三条经验**:① `NETPKT_BUFLEN = CONFIG_IOB_BUFSIZE = 196`,整帧必分片,`netpkt_getdata()` 只能看到第一片,必须 copyout;② 链路状态只有 supplicant 收到 `WPA_CTRL_EVENT_CONNECT_IND` 才会推进,本移植把 `wpa_ctrl_event_copy` 桩改成转发到 glue 的事件桥(`bk7258_wifi_wpa_event`),CONNECT_IND/DISCONNECT_IND 用开源的 `mhdr_set_station_status()` 置状态、断开时降 carrier —— 不桥接则关联在空中成功而状态永远 IDLE;③ ping 需要 `CONFIG_NET_ICMP_SOCKET`(文档没提)+ `CONFIG_SYSTEM_PING`(`NETUTILS_PING` 只是库),DHCP 走 `ifconfig wlan0 dhcp`(`CONFIG_NETUTILS_DHCPC`)。

   **WPA2 与 WPA3-SAE 均已端到端跑通(2026-08-18 真机,fbd9db6 + 后续)**。SAE 首连曾以 STKOF(CFSR bit20,Armv8-M 硬件栈限位)炸在 wpas 线程 —— 厂商 5120 字节栈是按 wolfssl 捷径算的,mbedtls 的 P-256 要 15K(厂商注释自己写了),OSI 已把 wpas 线程栈下限提到 16K;厂商的 FreeRTOS 上同样的溢出是静默堆腐蚀,STKOF 是移植做得更好的证据。:`CONFIG_BK7258_WIFI_WPA` 编入厂商 wpa_supplicant 全套 78 文件 + mbedtls 32 文件子集,`wapi psk` + `wapi essid` → 四次握手 → carrier 在 `WPA_COMPLETED` 才抬 → 加密链路上 DHCP + ping 公网 3/3。要点:supplicant 自建 VIF(我们不再预建);扫描走 `wlan_sta_scan_once()`、结果从 BSS 表取一次缓存到下一轮(`wlan_sta_scan_result` 每次调用后冲表);完成信号靠 supplicant 发的 `EVENT_WIFI_SCAN_DONE` 落在我们的 `bk_event_post` 桥上;`sa_station_init()` 必须先于 `wlan_sta_enable()`(否则信道表全空,`freq 0xaaaa`);`aes_encrypt/aes_decrypt` 编译期改名避开 NuttX libcrypto;全部随机数改绑 `arc4random_buf()`(厂商原树是 `rand()`,nonce/SAE 标量都从这儿出)。`wapi psk` 的 `iw_encode_ext` 解析 bug 已随手修掉。

   **尾巴已清(2026-08-18 真机)**:`ifdown` 真实断开+降 carrier(厂商栈无 deinit 路径,保持运行);`ifup` 幂等(`bk_wifi_init` 一次性,重入会在活线程上重建队列);`wapi disconnect` 实测能停掉 supplicant 的连接重试循环;RSSI 列修正 —— `ApPower` 声明写着 0-100 实为**原始 dBm**(ctrl_iface.c:295 直存 `bss->level`),按文档反推曾低 60-80dB;`wapi auth` 现在按 `param.flags` 的索引存值。**测试注意**:nsh 没有 `ifconfig wlan0 down`,是独立的 `ifdown`/`ifup` 命令,"down" 会被当地址解析。

   三个必须做对、做错都不报错的接缝(全部已实现,见 `chip/bk7258_wifi_shim.c` 头注释的 REAL/ADEQUATE/PENDING 分类):
   - **`bk_wifi_init()` 带 config 参数**。手写 `extern int bk_wifi_init(void)` 能编能链,厂商那句专防此事的 `config->os_funcs == NULL` 检查会被寄存器残留值躲过,故障出现在三十层之后的闭源代码里。凡是厂商 API 一律用它自己的头,别凭记忆写原型。
   - **`g_wifi_os_funcs`(211 项适配表)会被 `--gc-sections` 回收**——没有任何代码按名字引用它,闭源库也不引用。`WIFI_DEFAULT_INIT_CONFIG()` 宏引用它是唯一的存活理由。它活过来之后未定义符号从 44 涨到 99,那不是倒退,是真实集成面终于可见。
   - **PHY/RF 适配器必须先注册**。`bk7258_phy_osi.c` 里的表是 BLE 时期就写好的,但只有 `bk7258_ble.c` 调初始化,而 xts 配置不含 BLE。不注册则 `libbk_phy.a` 的 `rf_open_handler` 从 `.bss` 取到 NULL 表、按偏移 16 跳转。现已在 `bk7258_wifi_ifup()` 里带幂等保护地调用。

   两个极性陷阱(实现处有注释):电源寄存器 `0x44010040` 的位是**掉电位**,开电源要**清位**;电源子模块不是域,`PHY_WIFI = 201`,除以 `PM_MODULE_SUB_POWER_DOMAIN_MAX`(20)才得到 PHY 域号 10。

   ⚠️ **`CONFIG_WIFI_MAC_SUPPORT_STAS_MAX_NUM` 那条 ABI 风险已排除**:实测 `g_wifi_mac_sta_max_num = 2`,与闭源库 `me_strategy_mem_init()` 的分配式 `2*568+1136` 一致,值来自 `sdkconfig.h`(在我们 `-I` 首条上),不需要手工对齐。

   **openvela 侧那一半**:`chip/bk7258_wifi.c` 注册 `wlan0`,`netdev_ops_s` + `wireless_ops_s` 两张表齐全,`essid`/`bssid`/`passwd`/`mode`/`auth` 是真实现。**`CONFIG_DRIVERS_IEEE80211` 必须开**——官方网络驱动指南没提,不开则 `netdev_register()` 里 `case NET_LL_IEEE80211` 被编掉、返回 `-EINVAL`。指南另有两处与代码不符:`netpkt_setdatalen()` 实际返回 `int`,且**不存在** `netdev_lower_quota_set()`(配额直接赋值 `dev->quota[]`)。

   RTC 与 Watchdog 已补齐并真机验证（`/dev/rtc0` + `/dev/watchdog0`，见 PORTING_NOTES 十四章）。注意 1.3.15 看门狗用例还要求**咬狗后复位原因报 `BOARDIOC_RESETCAUSE_SYS_RWDT`**，`src/bk7258_reset.c` 已实现读回路径，但该映射**未上板验证**。

   RTC 有两条限制要知道：计数器不跨复位，墙钟时间掉电或重启即丢（`havesettime()` 如实返回 false）；AON 计数率是启动时实测判定的（这块板子是 32000 Hz 内部 ROSC，不是 32768 晶振），改动 `bk7258_rtc.c` 时别把它换成编译期常量。
4. **中断绑定只用 `irq_attach`。** 官方指南第二章的 `irq_attach_thread` / `irq_attach_wqueue` 一处都没用（两者在本仓 NuttX 里是可用的）。BLE/BT 那三条是**刻意**如此——对齐厂商 `bk_int_isr_register` → `NVIC_EnableIRQ` 的语义，链路层 ISR 本来就该在中断上下文，会阻塞的定时器回调已经走 HPWORK 了（PORTING_NOTES 十三章）。别顺手改成工作队列。
5. **核间中断与安全属性对本移植 N/A，不是遗漏。** `up_trigger_irq` 未实现（`SMP_NCPUS=1`，NuttX 只跑 CPU0；MBOX0/1 中断号已定义未接）；`up_secure_irq` / `up_secure_irq_all` 由 `armv8-m/arm_secure_irq.c` 提供，受 `CONFIG_ARCH_TRUSTZONE_SECURE` 门控，未开。
6. **指南本身有几处错，别照抄。** `up_disabled_irq()` / `up_enabled_irq()`（正确是 `up_disable_irq` / `up_enable_irq`）、`irq_attach_work()`（正确是 `irq_attach_wqueue`）、`CONFIG_ARCH_MINIMAL_VECTORTABLE_DYNAMINC`（拼写错，正确是 `_DYNAMIC`）；`up_irq_is_disabled` 和 `irqstate()` 在 Cortex-M 分支根本不提供，那是 Cortex-A/R 的接口。

   中断子系统逐条核对的完整记录见 PORTING_NOTES 十五章，其中三个 `NVIC_SYSH_*` 宏被 `nvicpri.h` 静默覆盖那条尤其值得看——**芯片层 irq.h 不要定义 `MAXNORMAL` / `DISABLE` / `SVCALL` 三个优先级宏**，公共层会覆盖且不告警。

## 八、纪律

- **公共仓零改动**：`nuttx/`、`packages/`、`vendor/` 一行不能改，全部改动落在本仓。
- **硬件事实要有出处**：寄存器、中断号、引脚复用值必须标明来自 Datasheet 或 `bk_idk` SDK，不靠推断。
- **画面/行为只认真机**：编译通过不等于跑通，结论以真机取证为准。
- **结论变了，先改标题和数字，再追加取证**：新发现总是往段落下方追加，上方的小标题、百分比和「尚未/已知/不支持」这类断言没人回头改——而读者是从上往下读的，看到第一句就走了。2026-08-29 核文档，13 处过时里 **5 处是同一节内部前后矛盾**：小标题写「跑到 `mm_init`」而它下面第一句就是「该记录已过时」；同一节里 `configs/nsh` 的 flash 占用同时写着 45.09% 和 95.16%（后者是 app 分区扩容前按 1728 KB 算的）；「`/dev/oneshot0` 尚未做」与「1.3.13 已通过」并排站着。**推翻旧结论时先划掉旧断言、改掉旧数字，再往下追加新证据；改完 grep 一遍同一节里的百分比和状态词。**
- **加新 CONFIG 后必须 `distclean` 再编**：cmake 只在初次配置时把 defconfig 展开成 `.config`，之后的 `olddefconfig` 拿的是已有 `.config`，**新加的行会被静默无视**（构建照样成功）。编完 grep 最终 `.config` 确认，比对 `cmake_out/<board>/defconfig.orig` 能看到 cmake 实际吃进去的快照。
