# BK7258 移植笔记：外设战役实录

接续 [README](README.md) 的芯片层启动史（bootloader 交接、内存暗礁、控制台
疑案），本文记录控制台通车之后的全部外设 bring-up：每个坑按
**现象 → 取证 → 根因 → 修法 → 教训** 展开。所有结论均在真机
（`/dev/cu.usbserial-310`）上验证，画面类结论以肉眼确认为准。

## 〇、方法论（先说规矩）

整个移植过程立了四条规矩，每条都是用翻车换来的：

1. **证据优先于推理。** 寄存器行为以真机探针和厂商 SDK 源码为准，
   数据手册和网上资料只当线索。
2. **一次烧录只改一个变量。** 违反过一次（putarea + QSPI1 同时上车），
   代价是一轮完整的二分排查。
3. **"传输完成"≠"字节正确"。** 引擎报 done、耗时合理、驱动无错，
   像素仍可能全错。画面验收只认肉眼。
4. **方位词先对齐参照系。** 用户说的"左眼/右眼"是机器人脸自身视角，
   与观察者相反。一次误读让排查多绕了一小时。

## 一、控制台 RX 卡死："he" 之谜结案

**现象**：NSH 能输出,输入 `help` 只回显 `he` 就永久卡死,复现率 100%。

**取证**：三次重写 ISR、去掉 WFI、中断栈加到 8192,失败字节序列
逐字节相同——排除栈溢出与竞态。加打印观察 `fifo_status`(0x18):
TX 忙时 RX 计数字段和 empty 位会**同时撒谎**,唯有 `rd_ready`(bit21)
与 `wr_ready`(bit20) 诚实。

**根因**：驱动以计数字段判断 RX 是否有数据,在 TX 排空期间读到假值,
提前退出取数循环;字符留在 FIFO 里,而边沿型中断不会再来。

**修法**：`rd_ready` 作为唯一出队判据;ISR 循环加预算上限与风暴保险丝;
另设 conmon 监控线程(平时静默)带 RX deadman,冻死时看门狗重启。

**教训**：状态寄存器的每个字段要**单独**验证可信度,"同一个寄存器"
不等于"同等诚实"。

## 二、复位的真相：看门狗是唯一的复位器

**现象**：`SYSRESETREQ`(NVIC 标准软复位)在本 SoC 上无效;没有复位
手段就无法自动化调试。

**取证与根因**：AON 看门狗(0x44000600)是唯一能真正复位芯片的机关,
写法为 `0x5A0000|period` + `0xA50000|period` 两段提交。但在
`__start` 顶部就去碰 WDT 寄存器块会**总线卡死直接砖机**——连砖两次,
每次都要用户手按 RST 救活。外设块在时钟/电源域就绪前不可访问。

**修法**：控制台起来之后才武装看门狗;SysTick 每 tick 喂狗;
`board_reset()` 用 6ms 周期 + 自旋实现可靠重启。

**reboot 自动刷机的返工记**：bootrom 下载窗口只有几十毫秒。最初的
做法是"进程 A 发 reboot → 退出 → 进程 B 启动烧录器",进程接缝本身
就超过整个窗口宽度,**从来没有成功过**——几周里所有"reboot 刷机成功"
其实都是操作者恰好按了 RST,归因错误一直没被戳穿,直到用户明确指出
"从来都是我手动 rst 才行的"。修法是把 reboot 塞进烧录器内部:
`bk_flash.py --reboot` 在**同一个已打开的串口**上发出 reboot,下一
微秒就开始 LinkCheck 轰炸,零间隙。实测连续两轮全自动成功,探测数
(3354/3368)高度一致。教训:①毫秒级窗口容不下进程边界;②"成功"
必须核对归因,旁边有人按按钮时尤其要小心。

**教训**：`__start` 早期是雷区,任何外设块的第一次访问都要先问
"它的时钟域活了吗"。

## 三、Beken 家族约定：soft_reset 是电平,不是脉冲

`global_ctrl` bit0 名叫 soft_reset,**语义是"复位释放电平,1=放行"**,
不是自清脉冲。这个约定 UART/I2C/SPI/QSPI 四个块完全一致。

- UART 上第一次遇到,花了一天;
- I2C 上又栽一次:先写 0x01 再写 0x02,第二次写把 bit0 清零,
  等于把刚配置好的块**重新按回复位态**——必须一次组合写 0x03;
- SPI/QSPI 一次写对。

**教训**：IP 家族的约定要提炼成显式知识,同一个坑在每个新外设上
都会再挖一次等你跳。

## 四、NVIC 之前还有一道闸：SoC 中断路由矩阵

BK7258 是三核 AMP,外设中断先经 SoC 级 per-CPU 路由矩阵
(0x44010080 起,`cpu0_int_0_31_en` 等,位序号=NVIC 线号),
再到 NVIC。矩阵不开,`up_enable_irq()` 做得再对中断也永远到不了。
已把矩阵操作织进 `up_enable_irq`/`up_disable_irq`,上层无感。

## 五、灯、按键、马达：厂商固件是权威引脚图

数据手册不含板级布线。对原厂固件
(`Conversational-AI-IOT-Sample/device`)做深度分析拿到权威事实:

| 器件 | 引脚 | 备注 |
| --- | --- | --- |
| LED 红/绿 | GPIO40/41 | 推挽,高=亮 |
| 马达 | GPIO9 | 另需 GPIO52 拉高(共享 3.3V LDO) |
| 按键 S1/S2/S3 | GPIO13/12/8 | 低有效,内部上拉 |
| 背光 | GPIO25 | 高有效,双屏共享 |
| 屏1 SCK/CS/MOSI/DC/RST | GPIO2/3/4/5/45 | GSPI 槽0 / QSPI1 槽6 |
| 屏2 SCK/CS/MOSI/DC/RST | GPIO22/23/24/7/6 | QSPI0 槽3 |

接入标准 NuttX 框架:`userled_lower` + `btn_lower`(GPIO 中断
派发器做双沿模拟)+ `DEV_GPIO`(马达)。全部实机验证:灯闪、
马达震、按键事件上报。

**坑**：Kconfig 的 select 链断一环就**静默丢配置**——
`USERLED_LOWER` 需要 `ARCH_HAVE_LEDS`,`VIDEO_FB` 需要
`DRIVERS_VIDEO` 的 menuconfig 门。每次改 defconfig 后必须
grep 最终 `.config` 验证没被丢。

## 六、I2C：组合写与 clk_src

轮询主机驱动,两个硬件坑:

1. 状态寄存器写入必须**一次组合写**(`SM_INT|控制位`),分两次写
   第二次会把中断标志误清;
2. `clk_src` 必须为 3,写 0 引擎永远不走(手册未说明)。

引擎已验证(快速 NACK 正确),板上暂无可正向应答的从机
(加速度计疑为 SPI 模式,摄像头需先供 MCLK),留待后续。

## 七、屏幕战役:从 28.7 秒到 17fps 动画

双 GC9D01 160×160 面板,这是整个移植最曲折的一章,四幕:

### 第一幕:GSPI 点亮第一块屏,但慢得离奇

GSPI SPI1(0x45880000)按 SDK 配好后能画,但**有效 SCK 比编程值慢
约 130 倍**,全屏一遍 28.7 秒。寄存器逐位对照 SDK 无差异——
**至今未解**,已降级为备胎并记录在案。

### 第二幕:软件 SPI 点亮第二块屏

QSPI0 专属引脚(22/23/24)上先用位敲 SPI(缓存配置字地址,
3 store/bit)把第二块屏点亮——先保功能,再谈速度。

### 第三幕:QSPI 间接命令引擎

把 QSPI 的 cmd_c 命令块当"字节炮"用:首字节作 1-wire 命令,
其余走数据相位,即成字节精确的纯 SPI 波形。双实例
(QSPI0=0x46040000/cken bit20/时钟字 0x09;
QSPI1=0x46060000/cken bit21/时钟字 0x0a)。

### 第四幕:双屏全黑与 61 字缓冲 RAM

**现象**：双眼切 QSPI + putarea 整块写后,fb 计时 3.87/3.90s
一切"正常",但**两块屏没有任何图像**(背光亮)。

**取证**：二分——只禁 putarea 重烧,依旧全黑,putarea 无罪。
系统级写入(时钟位域、cken 位、引脚槽位)逐项对照盘上真 SDK
(`bk_idk/middleware/soc/bk7258`)全部无误。最后通读 SDK 的
`lcd_qspi_driver.c` → `bk_qspi_write` → `qspi_ll_io_write`,
看到那行天机:

```c
*((uint32_t *)(hw->fifo_data) + i) = *((uint32_t *)data + i);
```

**根因**：寄存器 0x40~0x7C 是 **61 字缓冲 RAM,按字递增寻址**,
不是单口 FIFO。我们把所有数据字都写到 0x40 一个地址,缓冲区只有
槽 0 被反复覆盖——引擎发出的是"最后一个字 + 60 字陈货"。
命令字节对、波形真、CMD_DONE 诚实,唯独数据全错:面板初始化
参数全是垃圾,自然黑屏。同时揭穿一个历史误判:**QSPI 路径此前
从未画出过任何像素**,之前亮的屏全是 GSPI/软件 SPI 的功劳,
3.9s 的"成功计时"只是 fb 示例里 usleep 的地板时间。

**修法**：`qspi_putreg(priv, QSPI_FIFO_OFFSET + i, word)` 一行,
外加删掉网上老资料抄来的 `dummy_mode=4`(SDK 写操作不设 dummy,
多余的 dummy 周期会把数据流整体错位)。

**教训**：本章方法论第 2、3 条的出处。另加一条:
**研究以盘上真 SDK 为准**——网上对老款 Beken 的分析在关键处
(FIFO vs 缓冲 RAM)是错的。

### 尾声:eyes 动画

`eyes` NSH 应用(`apps/eyes/`):程序化虹膜环 + 瞳孔视差 +
随机眨眼,每帧只重画前后两帧虹膜框的并集(~11KB)并以
`FBIO_UPDATE` 增量刷新。实测 267 帧/15 秒 ≈ **17fps**,
眨眼肉眼流畅。后续把 QSPI 源时钟翻倍(320MHz÷8)、系统 tick 提到
1kHz、putarea 改 8 行批发送、喂狗 10 分频(AON 慢总线税),实测
19→21fps、画面肉眼确认无损;render 9ms/flush 21ms 之外每帧仍有
~17ms 未归因开销,已列入未竟事项。

## 七点五、摄像头阶段一:三小时,三个假凶,一个真凶

**目标**:让 DVP 传感器在 SCCB 上应答(整个取流管线的第一块多米诺)。

**过程实录**(按翻车顺序):

1. 研究揭示 SCCB 不在猜想的 I2C0/GPIO20-21,而在 **GPIO42/43**;
   MCLK 需先行(GPIO27, auxs_cis 480÷20=24MHz)。
2. 首扫全空。自制"引脚翻转计数器"测 MCLK:0 翻转 →"时钟没出来"。
   连补 auxs 总闸、VIDP 电源域、jpeg/yuv/h264 时钟、修正同名异物的
   `cksel_jpeg`(reg0x08 bit30,**不是** reg0x0a bit14)——仍是 0。
3. 探针自校准暴露第一层假象:输出模式下 `gpio_config` 剥掉 INPUT_EN,
   **计数器从来没在工作**。修好后软件翻转可见(199/200),外设时钟
   依旧 0——于是"确认"时钟死了。
4. 三探员工作流深挖 SDK 全链:PM 路径无隐藏握手;pad 应交外设
   (func_en + 输出级关断),不能带 GPIO 输出级;盘上代码**不存在**
   reset/PWDN 翻转(GPIO28 零消费者,别折腾)。照改,仍 0。
5. `xd` 直读寄存器:分频/使能/电源/功能选择**全部正确落盘**——
   排除"写丢"。同时发现 pad 被一行残留代码反复改写(改代码要 grep
   残句!)。
6. **顿悟**:位敲 I2C(/dev/i2c2)也配在 42/43 且最后初始化——
   引脚的最终主人是它,而所有扫描都打在被抢走引脚的 `/dev/i2c1` 上。
   **换扫 i2c2:0x3C 应答。** 读 0xF0/0xF1 = 0x21/0x45——
   **GalaxyCore GC2145,2MP DVP,验明正身。**

**真相**:MCLK 从头到尾都在跑(传感器应答就是铁证);"0 翻转"全是
假阴性——**引脚输入锁存器看不见外设驱动的信号**,只能看见 GPIO 输出级
自己的翻转。三小时追杀的"时钟疑案"根本不存在,真凶只有一个:扫错总线。

**教训**:
1. 仪器先校准,再信读数;校准通过也只证明"能测你校准过的那类信号"。
2. 引脚所有权是时序问题——最后初始化的驱动赢。多总线共脚时先查主人。
3. 残留代码是幽灵写手:每次改寄存器序列,grep 全文件同名调用。
4. 反证优于正证:传感器应答一举证明了时钟、电源、引脚、总线四件事。

**现状**:GC2145 @ /dev/i2c2 地址 0x3c,MCLK 24MHz,电源 GPIO49。
阶段二(取流)的权威 init 表在 `bk_avdk_smp/ap/components/bk_peripheral/
src/dvp/dvp_gc2145.c`(用户已提供本地仓)。

## 七点六、摄像头阶段二:一张 14,983 字节的照片

阶段一验明 GC2145 后,取流管线一次打通:642 条 SCCB 寄存器(脚本从
`dvp_gc2145.c` 机器转录,自动跳过 `#if 0` 段,585+40+17 三表条数
逐一校验)→ DVP 12 引脚 → YUV_BUF 行乒乓(SRAM 顶部雕出 32KB 专区,
PSRAM 供不起行级带宽)→ JPEG 编码器(量化表必须软件载入,硬件无默认)
→ CPU 直排流 FIFO → FAT 落盘。`snap [路径]` 一条命令抓一帧。

**CPU 直排的教训**:第一版绕开 DMA 用 CPU 轮询 FIFO,产出的 JPEG
结构完好(SOI/SOF/EOI 俱全)却解不出图——标记链在首个 DQT 后断裂,
后面是 0xFFFF 空读垃圾。根因:**"FIFO 非空"≠"够一个 32 位字"**,
表头阶段编码器按字节涓流输出,按字读取吃进半满字。SDK 的 DMA 之所以
必须,是因为握手请求线按"凑满才请求"节拍——这不是软件能模拟的。

**GDMA 五连环**(每一环都是一次真机取证):
1. `panic "irq: 5"` 是 **BusFault 向量**,不是外设中断号(追过一圈 PWM0);
2. GDMA 文档基址 0x45020000 对 CPU0 炸机,**secure 别名 +0x10000000
   (0x55020000)才是我们的门**(设备 ID 读回 "GDMA" 签名);
3. 通道 8 是别人的:`secure_attr` 上电值 0xff = 通道 0-7 归安全世界,
   8-11 免谈——**通道 8 的写全部静默丢弃**(RAZ/WI),SDK"JPEG 固定
   用通道 8"只是它的软件分配策略;
4. **single 模式不受外设握手节拍**——通道一使能就"瞬间完成",必须用
   SDK 的 repeat 模式(5KB 块 + 目的环回);
5. 块上电即处于复位态(soft_reset 读 0),通道寄存器照常可写,唯独
   **引擎拒绝 enable**——按 `dma_ll_init` 序列释放(整字写 0 再置
   bit0)后一切就绪。

**终局**:传感器先出流、AEC 收敛 400ms 后武装 DMA + 编码器,首个 EOF
即成熟帧。实测多张(15,879 / 15,519 字节),**640×480 JPEG 主机解码
成功,肉眼可见真实画面**。

## 八、内存:从 42KB 堆到 370KB

链接脚本约束见 README(SP 下限 0x28032468、`.bss` 必须低)。
运行时用 `kumm_addregion((void*)_ebbnote, 0x28098000 - _ebbnote)` 把
SRAM4/5 并入堆,42KB → 370KB。起点取 `_ebbnote` 而不是写死地址,是为了把黑匣子
记录区(`_bbnote`,链接区上沿 128 字节)留在所有堆之外。
本树没有 `up_addregion` 钩子,挂在 `board_late_initialize`。

## 九、未竟事项

- GSPI 慢时钟之谜(备胎可用,不阻塞);
- 加速度计(疑 SPI 模式)探测;
- QSPI 实际时钟频率未示波器实测,17fps 的提速余量未兑现;
- 摄像头画质调优(AEC 时长/量化表/更高分辨率);
- eyes 帧循环的 ~17ms 未归因开销(render 9ms + flush 21ms + sleep 3ms
  却每帧 50ms;QSPI 时钟、tick 粒度、SNDBLOCK 批量、喂狗频率四个嫌疑
  均已实测排除,需逐段插桩再审);
- 电源管理、CPU1/2 AMP。

## 十、验证矩阵(全部真机)

| 子系统 | 验证方式 | 结果 |
| --- | --- | --- |
| 控制台 | 交互输入长命令 + 深夜浸泡 | 稳定 |
| 看门狗复位 | `reboot` 命令自动重烧循环 | 可用 |
| LED/马达 | 肉眼/体感 | 通过 |
| 按键 | `buttons` 事件上报 | 通过 |
| I2C | 总线扫描快速 NACK 时序 | 引擎正常 |
| 双屏 QSPI | `fb` 图案 + 肉眼 | 通过 |
| eyes 动画 | 15 秒计帧 + 肉眼 | 17fps,生动 |
| PSRAM 16MB | free 17.1MB + 五点自检 | 通过 |
| SD NAND | FAT 挂载/写读/重挂持久 | 通过,原厂表情资源在卡上 |
| 摄像头 SCCB | GC2145 芯片 ID 0x2145 读回 | 阶段一通过 |
| 摄像头取流 | snap 多帧 + 主机解码 640×480 + 肉眼 | 阶段二通过 |
| AON RTC | 计数率两次开机实测 32129/32130 Hz;`date` 读写回读 | 通过,判定 32000 Hz ROSC |
| 看门狗 `/dev/watchdog0` | `wdog`:喂狗 5 秒不复位,停喂 2.2 秒后真复位 | 通过,含反向对照 |

## 十一、原生表情播放管线攻坚:从 7fps 到 22.4fps

原厂表情素材躺在 SD 卡上,此前板子只能望卡兴叹。本章记录把
**SD 读 → 硬件 JPEG 解码 → DMA2D 变换 → 双屏 blast** 整条管线
在板上原生打通的六场硬仗。规矩不变:全部真机取证,画面只认肉眼。

### 第一战:硬件 JPEG 解码器(0x48040000)

**分工**:JFIF 头由软件解析,熵码交硬件。驱动的活就是把头里的表
灌进引擎、告诉它熵码从哪开始、等它吐 YUV。

**表格装载的寄存器细节**:

- Huffman 表以 `(code<<8)|value` 组合字逐项写入:DC-Y@0x200、
  DC-UV@0x300、AC-Y@0x400、AC-UV@0x800;四表的位长计数分别写
  @0x40/0x80/0xC0/0x100,计数字写 @0x158;
- DQT 不能照抄 JFIF 原值:须按 Arai IPSF 系数 ×8192 预缩放,且按
  **光栅序**(非 zigzag 序)写 @0xE00/0xF00;zigzag 映射表本身
  写 @0xC00;
- `BASE_FFDA` 寄存器填 SOS 熵码的起始偏移;`rd_len` 给
  `len+2048` 的**过读余量**,否则引擎在真实数据吃完前断粮;
- 完成判据:轮询 `INTSTS`(0x17C)bit8。

**约束清单**(超出即拒):Y 采样因子 0x21、色度 0x11、baseline
(无渐进)、全部表头须落在文件首 1KB 内。

**输出格式疑案**:文档声称的排列是错的,实测输出为 **VYUY 4:2:2**。
定案方式是与主机软解逐像素对拍:按 VYUY 解读累计误差 55,按文档
排列解读误差 44647——数字自己说话,文档所写为误。

**教训**:输出格式这类"文档一句话"的事实,也要走一遍逐像素取证。

### 第二战:DMA2D(0x48080000)——不肯锁存的 DONE

硬解出的 320×160 VYUY 要变成两块 160×160 RGB565(左右眼各一屏)。
用 M2M_PFC 模式 + `fg_line_offset=160` 开窗,一次变换取半幅,
跑两次即完成拆分,无需中间整幅缓冲。

**现象**:轮询 DONE 状态位等完成,它永远不来,整帧时间预算全烧在
等待上。

**根因**:**DONE 位只有在对应中断使能置位时才会锁存**——使能
关着去轮询它,是在等一个永远不会写入的位。

**修法**:改以 **start 位自清**为完成判据,一次到位。

**输入格式标定**:fmt 字段不靠猜,在板上把 fmt0-3 × byte_reve
八种组合各跑一遍,对照 ffmpeg 主机真值(NEUTRAL 表情首帧背景
米色 de96)比对,一轮定案:fmt0 正确。

### 第三战:QSPI 映射模式与被偷走的四分之三时钟

把 QSPI 从"字节炮"升级为内存映射窗口,CPU 往窗口存字即上屏。

**进出序列**(顺序敏感):

- 进入:清 cmd_c 四寄存器 → `cmd_a_cfg2=0x80000000`(1 线数据)
  → config 依次置 `force_spi_cs_low`(bit6)→
  `io_cpu_mem_sel`(bit22)→ `disable_cmd_sck`(bit16);
- 退出:严格逆序,且退出前必须先等 FIFO 排空
  (status bit16 空、bit14 忙)。

**CLK_RATE 疑案**:`CLK_RATE(2)` 自映射模式点亮之日起就把 SCK
**悄悄除以 4**,名义 40MHz 实际只跑四分之一。换 `CLK_RATE(0)`
(厂商映射模式取值)后,播放从 7fps 直接跳到 22.4fps。时钟源为
sys div7 = 320MHz/8 = 40MHz。

**GDMA 搬运未遂**:让 GDMA 往映射窗口写,屏上始终无显示——单拍
目的、SEC 极性、cmd_c 清零皆试过,未解,记档。绕行:CPU
**交错双窗存储**,实测墙钟时间等于单屏线时间,第二块屏几乎免费。

### 第四战:面板线序——三色带钉死字节序,顺手结案"雪花"

**现象**:映射模式下颜色错乱。

**取证**:`WIRE` 三色带实测——喂红显蓝、喂绿显红、喂蓝显绿,
一次测量钉死。

**根因**:映射引擎按**内存低字节先行**串行化,而 GC9D01 把先到的
字节当像素高位——RGB565 的两个字节在线上颠倒了。

**修法**:blast 循环逐字 `REV16` 补偿,与 putarea 路径的手工
换字节殊途同归。

**意外收获**:此前偶发的"雪花"随线序修复一并消失。字节错序把
JPEG 低位噪声放大成高位色彩跳变,才是雪花的真身;40MHz 线路本身
干净,静止画面无雪花为证。

### 第五战:按键悬案——寄存器无罪,事件失踪

**取证**:裸探针 `KEYS` 证明三键引脚与原厂一致:GPIO13=<<、
GPIO12=电源、GPIO8=>>,低有效、内部上拉。

**悬案**:`/dev/buttons` 在播放中始终读不到事件。open 成功
(fd=5)、寄存器位定义与厂商 gpio_struct.h 逐位一致、btn_read
走的是活电平直读——三条线索都说"应该能读到",事实是读不到。
**根因未明,存疑待查。**

**工程绕行**:播放器放弃 /dev/buttons,改为每帧裸读 GPIO 做
边沿检测,连续帧计数去抖:切换表情判 2 帧、退出判 3 帧
(≈135ms 长按)。误触不再让播放静默退出;播放失败也改为回落
待机,而非整个应用退出。

### 终局:22.4fps 与通往 30fps 的两条路

管线全通:**SD 读 → 硬解 → DMA2D → 双窗 blast**,每帧 45ms、
稳定 22.4fps,CPU 240MHz。原厂 20fps 表情素材已可原速播放。

通往 30fps 的余量留档两条:

1. QSPI 时钟源 div7 → div3(40→80MHz),需重验高频下雪花
   是否回归;
2. 读/解/显三段流水线重叠。

## 十二、外设第二波:马达、扬声器、麦克风

显示管线收官后,继续向"会听会说会震"推进。三条通路全部
裸寄存器直驱,配方由三路并行侦察从 bk_idk / bk_avdk_smp /
原厂样例交叉考证而来——但每一路都在真硅片上翻过侦察报告的案。

### 振动马达(PWM3)

原厂操作点:PWM unit0 TIM2 驱动 GPIO 9(第二功能 1),
1kHz / 30% 占空、高有效。三个要点:

1. **CEN 位序是反的**:pwm_cr1 bit0=TIM3、bit1=TIM2、bit2=TIM1,
   按直觉写必错;
2. **通道成对共享定时器**:hw_ch2/ch3 同用 TIM2 的周期与占空,
   马达占了 ch3,ch2 就不能独立用;
3. **马达 LDO 轨(GPIO 52)与 SD 卡共用**:SD 在跑说明轨已上电,
   驱动只验证不开关——拉低它等于拔 SD 卡。

高脉冲位于周期尾部:CCR = period - duty。`face VIBE [ms] [duty]`
即测。

### 扬声器(片内 AUD DAC)

模拟侧全套照抄厂商 bring-up 值,每写一笔 ana_reg 都要轮询
0x440100E8 的对应忙位(内部 SPI 影子同步)。差分输出、仅左声道
有模拟驱动、模拟增益 0xA、数字增益 0x20。防爆音时序:先开 DAC,
30ms 后再拉功放(GPIO 50 高有效);关闭反序。

翻案一:**器件 ID 是小写**。0x47800000 读出 0x00617564
("aud"),不是寄存器手册暗示的 "AUD"——探活按大写值判会误报
无设备。

翻案二:**立体声 WAV 就是 FPORT 的原生格式**。交错 L/R 小端对
= 一个 (R<<16)|L 字,整字直灌;单声道才需要复制到两半。原厂
提示音实测混有 2ch 与 1ch 两种,都要支持。

### 麦克风(片内 AUD ADC)——三宗罪

`face REC` 从"FIFO 永远空"到满量程语音,连破三案:

1. **ADC 只认 APLL**。26M 晶振喂 DAC 好好的,ADC 的 Σ-Δ
   调制器却一动不动——家族默认 clk_src=AUD_CLK_APLL 早已言明。
   APLL 上电序列:ana5 掉电位清零 → 校准字 0x8973CA6F →
   配置字 0xC2A0AE86 → spi_trigger 脉冲 → apll_sel + cksel_aud
   切换。切完 DAC 也照常工作。
2. **共用寄存器必须 RMW**。AUDIO_CONFIG 与 FIFO_CONFIG 里
   ADC/DAC 字段犬牙交错,DAC 侧整字写会踩掉 ADC 的采样率与
   阈值——反之亦然。
3. **模拟 PGA 要用产品值 8,不是头文件默认 0**。audio_para.c
   的 mic0_analog_gain=0x8 才是 genie 出厂真身;这一档之差 =
   底噪 ±600 与语音峰值 29344 的天壤之别。侦察报告被 AVDK
   头文件默认值骗过一次。

MIC1(左声道)接板载麦;MIC2 实测静默,是喇叭回环的硬件回声
参考(genie 硬件 AEC 用),纯录音丢弃即可。录音循环带饿死超时,
永不悬死整板。开录提示用扬声器 1kHz 哔声(16 采样/周期查表
合成)——马达短震在桌面上感知不到,被用户当场证伪。

`face REC [sec]`:哔声 → 录音 → 存 /mnt/REC.WAV → 复读;
`face MIC [sec]` 是芯片自带 loop_adc2dac 位的实时直通,仅作
诊断——产品形态先录后放,录时不放,天然无回声。

### 板上还剩什么

SARADC、RTC/低功耗、USB、H.264 编码器、双副核 AMP、
Wi-Fi/蓝牙(最大的一座山)。音频闭环 + 表情引擎 + 按键 + 马达,
已足够撑起"会看、会听、会说、会震"的完整演示。

## 十三、无线:让闭源 BLE 协议栈在 NuttX 上跑起来

### 2026-08-03 结案更新（以下旧排障记录保留作历史）

本节后半段“发射未拿下”的结论已经过期。最终适配改用
`bk_avdk_smp` 中与 `projects/bluetooth/polar` 同代的
`libbluetooth_controller_controller_only_ble.a` + `libcom_phy.a`，并完成
新版 BT/PHY/RF OSI ABI、Polar KMOD 校准与默认功率表适配。旧版完整
Wi-Fi/BT 校准不再进入控制器启动路径。

最后一个真凶是共享 PHY 电源票。厂商 PM 把 `PHY_BT`（200）和
`PHY_RF`（202）作为同一父域的两个独立请求方；原移植直接把两者折成
父域 bit 10，RF 仲裁器每次 `RF_CLOSE` 都会在控制器仍持票时断掉 PHY。
现在由 `bk7258_phy_power_vote()` 维护引用票，只有最后一票释放才关域。

后续手机复测又抓到第二个 CP/AP 代际差异：新版 CP 控制器的
`_bt_rf_pll_ctrl(apply, rf_mode, rf_pll, priority)` 不是旧 AP SDK 中可在
无 Wi-Fi 时省略的“Wi-Fi PLL hold”回调，而是 BLE 自己申请/释放 RF
路径的入口。返回成功但不调用 `libcom_phy` 会造成 HCI 仍报广播开启、
射频在后续切换后却保持关闭。该回调现已按 AVDK CP 参考实现转发到
六参数 `rf_pll_ctrl()`；真机启动可见 RF 配置经过 `0x202/0x212` 仲裁，
连续观察及广播关闭/重开后均能继续收到空口数据。

真机 `/dev/cu.usbserial-310` 已验证 100 ms、三信道的 legacy
`ADV_SCAN_IND` 广播。macOS 上执行 `blew -t 20 scan` 可发现
CoreBluetooth 设备 `204157A4-E9D9-6F4A-5052-1C3B04C61F42`（近场约
-42～-53 dBm）；发送 `face BT 12` 关闭广播后该记录消失，再执行
`face BT 5` 后恢复。控制器侧广播载荷包含 `openvela-EPG` 与 16 位服务
UUID `FFF0`，但当前 macOS CoreBluetooth 回调没有上报这两个字段，
所以 `blew` 将名称显示为 `(unknown)`；开关对照 UUID 是目前可靠的验收
标识。

Android 版 nRF Connect 可按静态随机地址 `C8:47:8C:25:20:26` 查找；
iOS/CoreBluetooth 不向应用公开真实 BLE 地址，只会显示系统生成 UUID，
且本广播当前可能显示为无名称设备，因此不能在 iPhone 上用 MAC 过滤。

前面每一个外设都是裸寄存器重写。无线不行——Wi-Fi 的 MAC/PHY 和
蓝牙的链路层都是预编译 `.a`,没有源码也没有可下载的固件 blob,
代码直接链进镜像在本核执行。但厂商留了正门:闭源库与 OS 的全部
耦合收敛在**三张函数指针表**里(蓝牙 OSI 96 项、PHY 135 项、
RF 11 项),这是给"换 OS"预留的接缝。移植的本质是重新填表,
不是逆向。

先摸清战场再动手:三路并行侦察从 bk_idk / bk_avdk_smp / 原厂样例
交叉考证,确立了四条关键事实——单核 CPU0 就够(厂商自己也把
Wi-Fi/BT 全跑在 CPU0,CP1/CP2 明确关闭,不需要 mailbox IPC);
射频校准数据在 flash 片尾 0x7fe000 加 OTP 双备份,我们烧在
0x11000 没踩到;闭源库是 armv8-m.main / fpv5-sp-d16 / 硬浮点,
与我们的构建 ABI 恰好一致;蓝牙有干净的 HCI 边界。

### 第一战:链接闭源库,和一个把板子变砖的减法

三套库进镜像只需要补两样东西:八个库直接调用(而非经表注入)
的符号——beken 的 `rtos_*` 临界区/互斥原语、微秒延时、两个厂商
钩子;以及板级射频功率表,闭源 PHY 按名字和布局直接绑定,从
bk_idk 逐字抽取、类型在本地复刻。

然后板子死了,一个字节的外来代码都没跑到。真凶是闭源库那 37KB
的 `.bss`:IDLE 栈顶是 `_ebss + CONFIG_IDLETHREAD_STACKSIZE`,
一个纯算术表达式,链接器从不做区间检查。`.bss` 一涨就把栈顶顶出
了 SRAM 区界,于是 `up_allocate_heap()` 里 `_eram - g_idle_topstack`
算出负数、回绕成约 4GB,`nx_start()` 里分配器当场瓦解,控制台
一个字都没有。

修法把侦察报告要求的 SRAM 扩容一并办了:区间从 128K 扩到 192K
(0x28050000,正是二级堆区的起点,中间那 64K 本就闲置),另加
一条链接期 `ASSERT`——以后再撞这个坑是构建错误,不是砖。

### 第二战:填表,以及一个假的"构建成功"

96 项 OSI 表:49 项真实现(小环形队列带计数信号量、kthread、
nxmutex/nxsem、HPWORK 定时器、中断注册与路由、BTSP 电源域与
BTDM/XVR 时钟门、GPIO),14 项转发进闭源 PHY,33 项 stub
(ATE/DUT、btsnoop、UART HCI 透传、全部 Wi-Fi 共存钩子——
纯 BLE bring-up 一个都走不到)。

两个判断值得记:定时器回调走 HPWORK 而非 wdog,因为厂商的定时器
是 FreeRTOS 软件定时器、回调跑在定时器守护任务里,控制器可以
合法地在里面阻塞,而 wdog 的中断上下文会把这变成断言或死锁;
线程优先级映射 `200 + (9 - beken)`,把 beken"数值越小越紧急"
的约定反转到一个高于应用线程、低于 HPWORK 的频段。

期间踩到一个隐蔽陷阱:`--gc-sections` 把整个对象连同它对闭源
符号的全部重定位一起丢掉了,**第一次"构建成功"其实什么都没链上**。
让探针函数取一下地址才算数。

表被接受的那一刻是可验证的:版本 `0x00010001` 加结构体尺寸的
双字段握手通过,说明 96 项布局逐字节正确。

### 第三战:射频表,以及一次比对头文件更硬的验证

控制器起来后在 `xvr_init` 里 bus fault——射频投票落进
`libbk_phy.a`,而那个库自己的适配表还是空的。填完 131 + 11 +
53 + 4 项之后,控制器一路走到 `enter normal mode`。

这次的布局验证方式值得记:不是对着头文件核对,而是**反汇编闭源
库自己的代码**——`rf_module_vote_ctrl` 取偏移 `0x104` 调用日志
函数,而链接后的镜像里 `g_phy_os_funcs + 0x104` 正好是我们的
`phy_osi_log`。二进制自证,比任何头文件都可信。

代价要说清楚:这个移植没有 flash 校准读取器也没有接 SARADC,
所以闭源库跑在 `cali_ready_status:0x0` 状态,回退到板级默认功率表
(`bk7258_vnd_cal.c`)。**发射功率不是出厂标定值**,温补也关着。

### 第四战:三次把诊断做反

控制器暴露标准 HCI 边界,所以完全不挂 host 协议栈——四条 Core
规范命令直接经 VHCI 灌进去。全部返回 status 00。但**射频是死的**,
这条路上我连着三次给出了听起来自洽、实际是错的结论。

**第一次**:采到中断计数 0,断言"链路层没拿到中断"。错在采样点
在开发射之前,那时链路层本就无事可调度。挪到之后再采是 47 次/秒。

**第二次**:据此断言"射频在工作"。也错。中断在涨只证明链路层在
按时调度事件,**不证明有射频能量离开天线**。推翻它的是用户的手机,
以及一个本该更早做的自证测试:让板子**自己去听**。周围一定有 BLE
设备在广播,收得到就说明收发通路是活的。扫 10 秒,一个都听不到。

**第三次**:补上校准后崩溃,`CFSR` 显示 UsageFault,我说是除零的
特征。查证后推翻——这个平台 `CCR.DIV_0_TRP` 从不置位,整数除零
静默返回 0,根本不触发异常。更糟的是接下来那句:"INVSTATE 说明
不是空指针"。**完全推反了**:Cortex-M 的 `BX` 会把目标地址 bit0
装进 `EPSR.T`,目标是 0 就直接抬 INVSTATE 且**不取指**,所以
INVSTATE 恰恰是空函数指针在这个架构上的标准表现。而 `R3 =
0x00000000` 从第一份崩溃转储起就摆在那里,我三次都读成了别的。

### 第五战:一个空槽,和抄漏的那半个配置

真凶在链接后的镜像里一字不差:

```
nv_init:  ldr r3, [g_phy_funcs_t]
          ldr r3, [r3, #0x2c]      ← _nv_phy_reg_set_hook = 0x00000000
          bx  r3                   ← INVSTATE
```

而 `calibration_main` 的第三条语句就是 `bl nv_init`。

**为什么表上正好缺这一格**,是这场战役里最值得记的一课。厂商源码
在"关 Wi-Fi"时把这一格填 NULL,我们照抄了,**抄得对**;但厂商关
Wi-Fi 时链接的是 `libcom_phy.a`,它的 `nv_init` 只有一条 `bx lr`,
根本不读这一格。而我们**两个库都链**,链接顺序里 `libbk_phy`
(Wi-Fi 版)在前胜出,于是 Wi-Fi 版的 `nv_init` 去调了一个"关
Wi-Fi 配置从来不填"的钩子。**配置抄对了一半,另一半在链接命令行
里。**

填上这一格,校准跑到底(`calibration_main over`、`xtal_cali:58`),
扫描从 0 条变成 **577 条**——接收机听见了整个房间。

### 第六战:顺序,以及谁给射频上电

但冷启动仍然是聋的。原因是校准的位置:厂商在启动控制器**之前**
校准,这个移植必须**之后**。实测干脆——先校准扫到 0 条,后校准
扫到 447 条。

差别在于**谁真正给收发器上电**。这个移植里是控制器,经 OSI 表抬
BTSP 电源域和 BTDM/XVR 时钟(这条路我们实现对了)。而校准自己抬
射频域走的是 PHY 表另外两个槽位,那两个实现没干成这件事。校准跑
在前面,整定的是一个没通电的模块。

治本是修那两个槽位以恢复厂商顺序;当前用实测有效的顺序,并在代码
注释里写明了原委。

### 第七战:发射,以及一个必须承认的缺口

`openvela-EPG` 的广播在控制器眼里一切正常——`LE_Set_Random_Address`、
广播参数、广播数据、广播使能,四条命令全部 status 0x00——但**没有
任何外部设备收到过它**。手机(安卓 + nRF Connect)按名字找、按 MAC
找,都没有。

这一段试过的、以及各自被证伪的:

- **发射功率**:库算出的信道索引是 46-52,是默认表的正常值,不是
  最低档。把 OSI 的 `_get_ble_pwr_idx` 回调钳到 ≥60(控制器每次
  按信道取值的必经路径,也是唯一"设了不会被覆盖"的地方)——无变化。
- **地址类型**:厂商固件源码里 `own_addr_type` 明确用随机、把 public
  注释掉了。照做,先设静态随机地址再开广播。控制器认真校验参数
  (漏设地址时回 0x12 参数无效),说明 HCI 层健全——但仍无人收到。
- **射频模式**:`rwnx_rfconfig` 读出 `0x101`,PLL 和角色都指向 Wi-Fi。
  一度强制 `_get_rf_mode` 返回 POLAR,后来发现**这套控制器库根本没有
  polar 的实现**(`ble_enter_polar_mode` 只存在于新版 AVDK),已撤销。
- **外置功放**:板级 GPIO 26/28 是普通输入和 I2S 时钟,这块板没有 EPA。

真正有价值的两条结构性发现:

**射频库链错了。** 厂商的纯 BLE 工程(`projects/bluetooth/*`)配置里
没有 `CONFIG_WIFI_ENABLE`,SDK 的选择逻辑对这种配置链 `libcom_phy.a`;
我们一直链的是 Wi-Fi 版 `libbk_phy.a`。两者导出 401 个符号完全相同,
差别只在实现,而差异恰好就在咬人的地方:Wi-Fi 版的 `nv_init` 会跳进
一个"关 Wi-Fi 配置永远不填"的空槽位(那次 INVSTATE 崩溃),而且它
默认让发射去用 Wi-Fi 的锁相环。

**合成器指向错了。** bk_idk 这套控制器只能走 IQ 模式,而 IQ 模式需要
一个真在运行的 PLL。厂商固件 Wi-Fi 蓝牙同开所以常亮,纯蓝牙构建下
必须显式索要——`rwnx_cal_set_rfconfig_BTPLL()` 正是 Wi-Fi 栈共存时
让出合成器的接口,调用后 `rwnx_rfconfig` 从 `0x101` 变成 `0x102`。

两项都改了,接收侧毫发无损(冷启动 874 条),发射侧仍然收不到。

### 第八战:host 路径,和一个定位到但没解决的坑

> **2026-08-29 结案更新：本节的"至今没有解释"已经解释，坑也已经不在了。**
> 崩溃是在 `libbk_phy.a` 还挂在链接线上时测到的，38 分钟后 `0979dda`
> 因为另一个原因把它换成 `libcom_phy.a`，此后 host 再没试过。今天在真机
> 上复测，host 强制链入后扫描正常、无 BusFault。完整取证见
> **二十二章**。以下旧排障记录保留作历史。

厂商唯一验证过的广播路径是走闭源 host(`gatt_server` 范例即如此),
我们一直绕过它直接灌 HCI。而且 VHCI 那几个入口函数定义在名为
`uart_controller_only.c.obj` 的对象里——名字直白地暗示它们是给
"纯控制器"配置用的,全栈构建里真正驱动链路层的可能是 host 那条路。

但**只要把 host 库链接进来(哪怕一次都不调用),射频就坏**:板子能
正常启动,一跑扫描就 BusFault,`BFAR = 0x33000001`。

逐条排除:内存没溢出(SRAM 42%、堆 38KB 正数、IDLE 栈顶与向量表
一致);三个库零重名符号;段表干净无孤儿段;镜像里**零个静态构造
函数**(`_sinit == _einit`)。

解析崩溃现场的返回地址,调用链是:

```
calibration_init → calibration_main → bk7011_cal_tx
                 → bk7011_cal_rx_adc_dlym → ... 
```

栈上带着 `0x4980c000`,正是 TRX 射频寄存器块。`BFAR = 0x33000001`
不是 TRX 地址本身,更像是**从未上电的 TRX 读回垃圾、再被当指针用**。

两个曾经很像答案、但被证据推翻的推断,记在这里免得重蹈:

- "厂商把控制器关键 `.bss` 收进专门的 `.sram_bt` 区,我们没复刻"
  ——那个区**只在 `CONFIG_BT_REUSE_MEDIA_MEMORY` 下存在**,厂商的
  普通构建同样让它们自由落位。不成立。
- "发射校准从未运行,所以发不出去"——反汇编 `calibration_main`
  的调用图,`bk7011_cal_tx` 明明在里面,两个构建都会走到。不成立。

**当时没有解释**(已于二十二章结案):为什么同一份校准代码,不链 host 时跑得好好的
(冷启动扫描 874 条),仅仅把 host 库链进来、一次都不调用,就会
在这条路上 BusFault,而且每次都落在同一个非法地址。没有重名符号、
没有构造函数、没有孤儿段,剩下的机制只能是链接 host 导致控制器
归档里被额外抽取了对象——但具体是哪个、怎么影响的,没查清。

### 现状与余量

闭源 BLE 协议栈在 NuttX 上跑通了,**接收完全工作,发射未拿下**。

接收是硬证据:冷启动无需任何手工步骤,扫描 10 秒稳定收到 700~870 条
广播报告、十余台设备,信号强度 -52~-76 dBm 合理,设备名能正确解出
(认得出周边的 midea 家电)。控制器启动、三张适配表、SARADC、射频
校准、HCI 收发全部就位。

发射是缺口:所有 HCI 命令返回成功、链路层按时调度广播事件、射频
配置已切到蓝牙自己的锁相环、用上了厂商为纯 BLE 准备的射频库——
但没有任何外部设备收到过我们的广播。

余量按性价比排序:

1. **先解决"能自己验证发射"这件事**。整段排查里最大的成本不是
   改代码,而是每次都要请人拿手机扫一次。macOS 自扫试过但被 TCC
   拦住(蓝牙访问归属到 Python 所在的 app 包而非 Claude,补 Info.plist
   并重签、换 bundle 标识、刷 LaunchServices 全部无效)。装一个现成
   的 BLE 扫描 App,或换台机器,都比继续盲试划算。
2. **复刻厂商的 `.sram_bt` 内存分区**,然后走 host 正路——那是厂商
   唯一验证过的广播路径,现在被上面那个布局坑挡着。
3. 出厂标定数据读取(需要 flash 读驱动;目前发射功率用板级默认表)。
4. **修 PHY 表那两个电源/时钟槽位**——修好可恢复厂商的"先校准后
   启动控制器"顺序,也是最后一处"知其然不知其所以然"。
5. GATT 服务;再往后才是 Wi-Fi(同一套打法但表大得多,208 项,
   外加 pbuf 语义和 lwIP 的取舍,是数量级更大的工程)。

两个已知隐患留档:PHY 电源域位在 OSI 表和 RF 表之间没有引用
计数,是"最后写入者胜",厂商用它自己的 PM 模块仲裁;模拟寄存器
写超时是静默返回,丢一次 `spitrig` 会让 DPLL 悄悄失锁而无任何
报错。

## 十四、RTC 与看门狗:补齐官方必测清单的两项

官方《新平台适配指南》把「通用自测用例」列为必测,内含 RTC 与
Watchdog。此前板上两样都没有驱动:看门狗只当复位器用(第二章),
RTC 连寄存器都没碰过。本章记两件事怎么补上,以及路上捡到的一个
本来会每天走快 35 分钟的坑。

### AON RTC:寄存器出处与一次必要的实测

寄存器偏移取自 bk_idk master `middleware/soc/bk7258/hal/aon_rtc_ll.h`。
那个文件有两套写法,取**显式字偏移**的一套,因为它无歧义,而且两套
互相校验:`tick_init()` 往 `+0x0*4` 写 0x40,而 `aon_rtc_ll_enable()`
定义使能位是 `(0x1<<6)` —— 0x40 正是 bit6,所以 `+0x00` 是 CTRL;
`open_rtc_wakeup()` 把比较值写 `+0x2*4` 再置 `BIT_AON_RTC_RTC_TICK_INT_EN`
(0x8 = bit3 = tick_int_en),所以 `+0x08` 是 TICK_VAL;`+0x0c` 是
`get_current_tick()` 读的自由计数器。CTRL 的 bit4/bit5 是 W1C 状态位,
任何读改写都必须把它们掩掉,否则会静默吃掉一次待处理中断。

**32K 时钟不能想当然。** bk_idk 的注释写明这个计数器要么挂外部
32768 Hz 晶振,要么挂内部 32000 Hz ROSC,取决于 bootloader 选了哪个。
两者差 2.4%,选错就是每天 35 分钟。所以驱动在 init 时拿系统 tick
量一次(先等到 tick 边沿再开窗,500 ms),再把结果贴到最近的候选值上。

真机结果:**实测 32129 Hz,判定 32000 Hz —— 这块板子跑的是内部
ROSC,不是晶振。** 两次独立开机测得 32129 / 32130,重复性 0.003%,
说明量法本身可信;偏离标称 +0.4% 则是 RC 振荡器该有的样子。

64 位计数在**软件**里扩展。硬件确实有 `_hi` 寄存器(见
`aon_rtc_hal_64bit.h` 的 API 签名),但偏移只存在于非公开的
`aon_rtc_hw.h`,公开仓里没有。本移植的规矩是不猜寄存器地址,所以
32 位计数器由软件补高位,代价是每次读多一次比较,外加一个远比
36.4 小时回绕周期密集的采样。

### 为什么 RTC 起在 board_late_initialize 而不是 up_rtc_initialize

`clock_initialize()` 会在 `nx_start()` 里很早就调 `up_rtc_initialize()`。
两个约束在这里撞上:AON 域在就绪前被访问会挂总线——第二章那两次
砖机就是这么来的;而测频又必须有跑起来的系统 tick。于是选
`CONFIG_RTC_EXTERNAL`:告诉 OS 时钟会晚点到,`up_rtc_initialize()`
只是个返回 OK 的桩,真正的初始化在 `board_late_initialize()`,那时
控制台已起、看门狗已经在同一个 AON 域里喂了一阵子,域是活的这件事
有据可依。`up_rtc_set_lowerhalf(lower, true)` 落地时顺手
`clock_synchronize()`。

**计数器不跨复位。** 实测:开机约 55 秒时 `date` 显示
`Thu, Jan 01 00:00:55 1970` —— 计数器在芯片复位后从 0 重新开始。
所以墙钟时间掉电或重启即丢失,`havesettime()` 如实返回 false。
要跨复位保时间得找 AON 保持寄存器,那又是一组没有公开出处的地址,
没做。

### 看门狗:让 /dev/watchdog0 不是装饰品

直接把 start/stop 转发给硬件是行不通的。AON 看门狗只有一个周期
寄存器,武装和喂狗是同一个写操作,而 SysTick 每 10 ms 已经在替
系统喂它(那是死机保命网)。转发上去的话,心跳会在用户态底下一直喂,
狗永远不会为用户关心的理由咬人。

所以 deadline 放在上一层:驱动把一个系统 tick 截止时刻交给心跳,
心跳在截止前照常喂,一旦过期就**主动武装 6 ms 的 boot 周期**。
主动武装而不是单纯停喂,是因为运行周期约 65 秒 —— 光停喂的话,
一个 2 秒的超时会被拖到 65 秒后才复位,那个超时值就成了谎话。
保命网一点没动:tick 整个停掉时,硬件照旧在运行周期后咬。

真机(`wdog`,超时 2000 ms,喂狗 5 秒,间隔 500 ms):

- 喂狗期间 `ping elapsed=0..4500` 全程无复位 —— 反向对照成立
- 停喂后 `NO ping elapsed=5000..6500`,末次喂狗后约 **2.2 秒**芯片复位,
  完整启动序列重来(psram → rtc → ble → camera → NSH)
- 2.2 秒对 2.0 秒标称,多出来的是心跳粒度(10 ms)加 bootloader 交接时间

`capture` 故意留 NULL:AON 看门狗直接复位 SoC,没有本移植能挂的
预超时中断,承诺回调就是撒谎,上半部会如实报告 WDIOC_CAPTURE 不支持。

### 顺手修掉的一个工具 bug

`bk_crc_pack.py --verify` 从来没通过过。4 KB 尾部护栏(第七章那个
ICache 预取悬崖)被无条件加在读文件之后、`--verify` 分支之前,于是
自检会给一个**已编码**镜像再补 4096 字节,然后以"长度不是 34 的
倍数"拒绝它。护栏属于编码路径,挪进去即可。修完 18894 块零失配。
README 里写的自检命令现在真的能跑。

### 教训

- **供应商注释里的"要么…要么…"是必须实测的信号,不是背景说明。**
  32768 是那个"显然"的默认值,而这块板子跑的是 32000。
- **防撕裂的"读两次直到一致"是任务级模式。** 计数器 30.5 µs 变一次,
  AON 总线又慢,这种无上限自旋不该进 tick 中断。
- **没有公开出处的寄存器就不用。** `_hi` 省下来的那点软件复杂度,
  不值得拿一个猜出来的地址去换。

## 十五、中断子系统:对着官方指南逐条过一遍

官方[中断系统适配指南](../../../docs/zh-cn/chip_porting/Interrupt_System_Adaptation_Guide.md)
的必做项本移植原本就都在(`up_irqinitialize` / `up_enable_irq` /
`up_disable_irq` / `NVIC_IRQ_FIRST` / `NR_IRQS` / 四档优先级宏),
`up_irq_save` 一族由 `arch/arm_m/irq.h` 白送。逐条核对时挖出四件事。

### 三个宏是死代码,而且是静默的

`chip/include/irq.h` 里原本定义了 `NVIC_SYSH_MAXNORMAL_PRIORITY` /
`NVIC_SYSH_DISABLE_PRIORITY` / `NVIC_SYSH_SVCALL_PRIORITY`,**三个
全部无效**。`arch/arm/include/irq.h` 先 include 芯片 irq.h(第 49 行)、
后 include `arch/arm_m/irq.h`(第 66 行),后者拉进的 `nvicpri.h`
无条件重定义这三个,所以后到的赢。

没有告警,是因为两个头都经 `-isystem` 进来,GCC 默认不报系统头里的
宏重定义。**用同样的命令行编一个 `#define FOO 1 / #define FOO 2`
是会报的**,所以别指望编译器帮你发现这类事;用
`-dM -E` 打预处理终值才看得见:

| 宏 | 板级写的 | 实际生效 |
| --- | --- | --- |
| `NVIC_SYSH_MAXNORMAL_PRIORITY` | 0x40 | **0x80** |
| `NVIC_SYSH_DISABLE_PRIORITY` | 0x40 | **0x80** |
| `NVIC_SYSH_SVCALL_PRIORITY` | 0x00 | **0x40** |

功能上一直是对的(外设中断全在 0x80,`up_irq_save()` 把 BASEPRI 抬到
0x80 正好全屏蔽,SVCall 在 0x40 不被屏蔽),但注释宣称的和跑的不是
一回事,而且 0x40 那一档成了"屏蔽不掉的准零延迟档" —— 将来谁把某条
中断提到 0x40,它就能在临界区里插进来。三个宏已删,由公共层统一给,
和主线 stm32l5/u5、nrf53 等 armv8-m 芯片的做法一致。

### 优先级是 3 位,不是 2 位

`__NVIC_PRIO_BITS` 在
`bk_avdk_smp/ap/components/cmsis/CMSIS_5/Device/Beken/armstar/armstar.h:59`
是 **3**(armstar = STAR-MC1),即 bits[7:5],八档。所以补上了指南要求
但原本缺的 `NVIC_SYSH_PRIORITY_SUBSTEP 0x20` —— bits[7:6] 做 group、
bit[5] 做 sub,和指南 rtl8720c 样板的划分一致。这不是抄格式,bit[5]
确实存在。

同时打开了 `CONFIG_ARCH_IRQPRIO`。在此之前 `up_prioritize_irq()` 写好了
但压根没编进去(`# CONFIG_ARCH_IRQPRIO is not set`),3 位优先级的硬件
能力一位都没用上。镜像成本 0 —— 暂时没人调用,`--gc-sections` 直接回收。

### 精简向量表:省 240 字节,附带一个要知道的副作用

`NR_IRQS` 是 76,而真正 attach 的只有 10 条:SVCall、HardFault、
SysTick、UART0、GPIO、RTC、DM、BLE、BT,加 MPU 打开时的 MemFault。
`g_irqvector[76]` 白占 608 字节。已开:

```
CONFIG_ARCH_MINIMAL_VECTORTABLE=y
CONFIG_ARCH_MINIMAL_VECTORTABLE_DYNAMIC=y
CONFIG_ARCH_NUSER_INTERRUPTS=24
```

实测 `.bss` 143248 → 143008。符号级:`g_irqvector` 608→192,新增
`g_irqmap` 76、`g_irqrevmap` 96。注意 `g_irqrevmap` 是 `int[NUSER]`,
指南没提这一块,所以别按指南的算法估收益。

**副作用要记住**:DYNAMIC 模式下 `irq_dispatch()` 走
`IRQ_TO_NDX()`,未映射的中断号会在**中断上下文里**调 `irq_to_ndx()`
分配一个槽位,并且 `DEBUGASSERT(g_irqmap_count < NUSER)`。也就是说
每来一次意外中断就永久烧掉一个槽,烧光了会在中断上下文断言。
24 对 10 留了 14 格余量,够用;但**将来加中断线时要把 NUSER 一起抬**,
而且如果开始追一个反复触发的意外中断,先想到这条。

### NVIC 之前那道闸,初始化时也要清

第四章讲过 SoC 路由矩阵。`up_disable_irq()` 一直是 NVIC 和矩阵两边
都清,但 `up_irqinitialize()` 原来只清 NVIC —— bootloader 交接时它
自己路由的线(至少 UART0)还留在矩阵里,两道闸对不上。已在初始化
循环里补 `putreg32(0, BK7258_SYS_CPU0_INT_EN(i))`。不丢功能:驱动
认领某条线时 `up_enable_irq()` 会把矩阵位再置回去。

### 教训:改了 defconfig 不等于 .config 生效

这次 `CONFIG_ARCH_MINIMAL_VECTORTABLE=y` 写进 defconfig、构建成功、
**`.config` 里却是 `# ... is not set`**,连编两遍都一样。原因是
cmake 只在初次配置时把 defconfig 展开成 `.config`,之后的 `olddefconfig`
是拿**已有的 `.config`** 跑的,新加的行根本没被读。`CONFIG_ARCH_IRQPRIO`
那次侥幸赶上了一次全量重配才生效。

排查这类事看 `cmake_out/<board>/defconfig.orig` —— 那是 cmake 当时
真正吃进去的快照,一比就知道。**加新 CONFIG 后要 `distclean` 再编**,
然后照第五章那条老纪律 grep 最终 `.config` 确认。

### 这一章的验证边界

以上全部是**构建级取证**:`.config` 取值、`nm` 符号尺寸、`objdump`
确认 `0x44010080` 进了 `up_irqinitialize` 的常量池、`-dM` 预处理终值。
**没有上板**。前三项(宏、SUBSTEP、IRQPRIO)预处理终值与改动前逐字节
一致,不改变行为;但**矩阵清零是真的动了启动时序**,碰的又正是当年
把控制台 RX 弄死的那套闸门,按本仓纪律得真机复验才算数。

## 十六、蓝牙上手机：两个回归，一条链路

**现象**：手机 nRF Connect 始终扫不到板子。这个症状挂了几周，期间的结论是
「所有 HCI 命令都返回成功、射频投票按广播事件周期变化、但空口没有信号」。

**取证**：把 OpenVela 蓝牙服务（ZBlue 主机 + `bluetoothd` + `bttool`）接上
之后，症状分解成三个可独立测量的问题，逐个上板证伪：

### 1. 每条 HCI 命令恰好 10.000 秒

驱动里加了带时间戳的有界收发轨迹（`face HCIT`），真机profile 一看就懂：

```
+20ms     > cmd 1003      +10010ms  < evt 0e     ← 整 10 秒
+10010ms  > cmd 1001      +20010ms  < evt 0e     ← 整 10 秒
+141280ms > cmd 0c13      +141280ms < evt 0f     ← 被拒的，0 ms
```

整数 10 秒不是硬件时延，是超时。根因在 `enter_normal_app_mode()` 的主循环：
`if (ble_ps_enabled()) rwip_sleep();` 然后才 `rwip_schedule()`，而
`ble_ps_enable_set()` 开机就无条件把它打开（忽略参数直接存 1）。厂商自己的
安排里是 UART 收发中断把芯片唤醒，这个移植的 HCI 走函数调用，**没有那个
唤醒源**，控制器睡下去只能等定时器。

**修法**：控制器初始化末尾 `ble_ps_enable_clear()`。适配器 enable 从 141 秒
降到 1.4 秒，`adv start` 从 `START_TIMEOUT` 变成 `status:0`。代价是空闲功耗。
`bk7258_bt_ps_keep()` 保留了两种都能测的开关。

### 2. AVDK 迁移打坏了射频（收发都坏）

`ff1d219` 把闭源归档从 bk_idk 换成 bk_avdk_smp，和 RTC/看门狗一起提交，
之后射频再没被验证过。同一块板、同一分钟、连续两轮：

| 归档 | `face BT 6` 接收（10 秒） | 裸 HCI 广播 |
| --- | --- | --- |
| bk_avdk_smp | **0** | 扫不到 |
| bk_idk | **2540–2649 个广播者** | **−53 dBm 扫到** |

排查途中排除掉的（每条都上板测过）：controller-only vs controller+host
归档（AVDK 树内换，仍 0）、省电开关（A/B 都 0）、HCI 驱动实现（手写字符
设备和公共仓 BTH4 给出逐毫秒相同的时序）、传统 vs 扩展 HCI 操作码（都 0）。

**注意**：单换归档回不去——`ble_enter_iq_mode` / `ble_enter_polar_mode` /
`txpwr_max_set_bt_iq` 是 AVDK 专有符号，C 代码和归档是绑定的，要连
`bk7258_ble.c` / `bk7258_bt_osi.c` / `bk7258_phy_osi.c` 的射频部分一起退。

### 3. 控制器接受扩展广播命令但不发射

退回 bk_idk 之后，同一个镜像上：

- 服务默认走扩展广播（`2036`/`2037`/`2039`），三条命令**全部 Command
  Complete 成功**，空口**无信号**；
- `adv start -m legacy`（传统 PDU），**手机立刻扫到**。

所以那三条扩展命令是被接受了但没有真正发出去。需要
`CONFIG_BT_EXT_ADV_LEGACY_SUPPORT=y` 且运行时选 legacy 模式。
`CONFIG_BT_EXT_ADV` 本身不能关——服务的
`sal_le_advertise_interface.c` 无条件调 `bt_le_ext_adv_*`，关掉直接链接失败。

**终局取证**：nRF Connect 扫到 `EPGSVC5432`，`C8:47:8C:25:20:26`，−69 dBm，
Advertising type: Legacy，LE General Discoverable。这个 MAC 正是几周里
一直在找、一直扫不到的那一个。

**教训**：
1. 一个症状可以是三个独立故障叠出来的。「命令成功但空口没信号」在三条
   都修完之前，看起来始终是同一个不可解的问题。
2. **归档迁移必须单独提交并单独验证。** 这次和 RTC 一起提交，直接导致
   射频回归几周没人发现。
3. 时间戳比推理值钱。10.000 秒这个整数一出现，「硬件慢」的整条假设链
   立刻塌掉——加时间戳之前我为此下过三次错判并逐一撤回。

## 十七、蓝牙服务上板：配置依赖清单

把 OpenVela 蓝牙服务（`frameworks/connectivity/bluetooth` + ZBlue）在本板
跑起来，卡点全是硬依赖，逐条记下省得重走：

| 缺口 | 解 |
| --- | --- |
| `BLUETOOTH` 依赖 `LIBUV_EXTENSION` | 在 `frameworks/system/utils/uv`，打开即可 |
| libuv 需要 `TLS_TASK_NELEM` | `CONFIG_TLS_NELEM=4` / `TLS_TASK_NELEM=4` |
| `UNQLITE` 依赖 `FS_LOCK_BUCKET_SIZE>0` | 设成 4（本树无 KVDB，存储只能走 UNQLITE） |
| zblue 纯 BLE 编不过 | `att.c` 在 `IS_ENABLED(CONFIG_BT_ATT_OVER_BR)` 里用了只在 `#if` 下定义的 `br_chan`；`BT_CLASSIC` 必须编译期开，运行期 `bt_br_init` 由控制器能力位门控会自动跳过 |
| `BLE scan + 服务` 绑死 socket IPC | `scan_manager.c` 无条件调 `bt_socket_server_is_busy()`，只能 `SOCKET_IPC` + `bluetoothd` + `NET_LOCAL`，`FRAMEWORK_LOCAL` 编不过 |
| PSA crypto 找不到 | zblue `select MBEDTLS` 选错了名字，真正的依赖是 `CRYPTO_MBEDTLS` |
| mbedtls 要熵 | 补 `CONFIG_DEV_URANDOM` + `DEV_URANDOM_XORSHIFT128` |
| `psa_crypto_init` 踩爆线程栈 | `SYSTEM_WORKQUEUE_STACK_SIZE` 4056 → 32768 |
| `/data/misc/bt` 建不出来 | 板级挂 tmpfs 到 `/data`（路径在服务里写死） |

**RAM 账**：蓝牙原始代价 +217 KB，其中 **`g_gatt_client` 一个符号 147.6 KB**
（框架侧按 8 连接 × 20 服务 × 100 特征静态开）。关掉框架侧 GATT 客户端 +
AVRCP/AVCTP + BT log 后降到 +60 KB。IDLE 栈从 74648 降到 16384（实测只用过
1288 字节）腾给工作队列。链接区 192K → 256K（SRAM2+SRAM3）。

**驱动走官方路径**：按官方
[如何添加一个蓝牙驱动](https://github.com/open-vela/docs/blob/dev-ai-contest-2026/zh-cn/device_dev_guide/connection/bluetooth/how_to_add_a_bluetooth_driver.md)，
`chip/bk7258_hci.c` 实现 `struct bt_driver_s`（open/send/close + 控制器回调），
`bt_driver_register()` 注册，`/dev/ttyHCI0` 由公共仓 `uart_bth4.c` 生成。
H4 拆帧、跨次写入重组、接收环、read/poll 全部属于 BTH4，不要自己重写一遍
——本移植早期版本重写过，行为一致但每换一个上层协议栈就要重新验证一次。
`head_reserve = 1` 让 H4 类型字节原地回填，不用拷贝。

## 十六、对着官方必测清单补缺口：/etc、复位原因、以及一个装不下的镜像

前一章把中断子系统对着官方指南过了一遍。这一章换个对象：**官方《新平台
适配指南》第五节把 xTS「通用自测用例」列为必测**，本章记录逐条核对的结果
和补上的东西。核对方式是机械的——把
`docs/zh-cn/test_dev_guide/openvela_xts_test_cases.md` 第一章里出现的所有
`CONFIG_*` 抽出来（68 项），和构建产物的 `.config` 做 diff，而不是读文档
凭印象判断。

### 文档里的符号名有三分之一在本树不存在

diff 出来的第一批"缺失"其实是**命名漂移**，照抄会写出永远不生效的行：

| xTS 文档写的 | 本树实际 | 结论 |
| --- | --- | --- |
| `CONFIG_CMOCKA` | `CONFIG_TESTING_CMOCKA` | 早已开启 |
| `CONFIG_PSEUDOFS_SOFTLINKS` | `CONFIG_FS_LINKS` | 早已开启 |
| `CONFIG_TESTS_TESTCASES` / `CONFIG_FS_TEST` | 不存在 | 见下 md5 |
| `CONFIG_TESTING_CRYPTO_3DES_XTS` | `CONFIG_TESTING_CRYPTO_AES_XTS` | 名字不同 |
| `CONFIG_DRIVERS_RTC` | 不存在 | defconfig 里的死行，已删 |

**教训**：跨仓抄配置项前先 `grep '^config X$' nuttx apps` 确认符号存在，
否则就是给自己埋一颗"写了但不生效"的雷。本仓 defconfig 里就躺着三颗：
`CONFIG_DRIVERS_RTC`（本树无此符号）、`CONFIG_PSEUDOFS_SOFTLINKS`（名字
不对）、`CONFIG_ARMV8M_SYSTICK`（`depends on TIMER`，而 `TIMER` 没开——
时基十四章起就换成 AON RTC 的 `arch_alarm` 了，这行是换过去时的残留）。
三行都静默无效，构建照样成功。

### `/etc` 根本不存在

官方指南第三节板级层清单里有一项 **ETCROMFS**，本仓从来没做。
`CONFIG_FS_ROMFS is not set`，`/etc` 不存在，而用例 1.1.12 要
`md5_test -f /etc/1.txt -c 100`。

补法照 `vendor/sifli` 的 SF32LB52（本树里最接近的参照）：`src/etc/` 放
`1.txt`、`group`、`init.d/rcS`、`init.d/rc.sysinit`，`src/CMakeLists.txt`
里 `nuttx_add_romfs()` 烘成镜像，`sched/init/nx_bringup.c` 在 init 任务
起来之前自动挂载——板级不用写挂载代码。产出的 `romfs.img` 是 1024 字节，
`strings` 能看到四个文件都在。

两个坑：`RCSRCS` 列出的文件**会过预处理器**，所以 `rcS` / `rc.sysinit`
里不能写 `#` 注释（sifli 的这两个文件是空的，不是偷懒）；`RCRAWS` 才是
原样拷贝。

`md5_test` 本身也得自带——文档要的 `CONFIG_TESTS_TESTCASES` / `FS_TEST`
在本树不存在，sifli 的做法是板级提供一个 `md5_test.c`，本仓照办
（`src/md5_test.c`，`CONFIG_CRYPTO` 提供 `md5init/md5update/md5final`）。

### 复位原因：从厂商 SDK 反查出寄存器

用例 1.3.15 除了咬狗，还要求 `boardctl(BOARDIOC_RESET_CAUSE)` 回报
`BOARDIOC_RESETCAUSE_SYS_RWDT`。本仓此前只有 `board_reset()`，没有
`board_reset_cause()`。

寄存器不靠猜。`bk7258.defconfig` 第 2 行写着 `CONFIG_SOC_BK7236XX=y`
——**BK7258 属 BK7236XX 家族，不是 BK7256XX**，这一步定错整章就废。据此
`middleware/driver/reset_reason/reset_reason.c` 走的是这个分支：

```
读:  aon_pmu_ll_get_r7a() >> 24 & 0x7f
写:  R0 的 [30:24] 读改写，再往 R25 依次写 0x424B55AA、0xBDB4AA55
```

R25 那对魔数是厂商的锁存序列（注释原文 "pass PMU_REG0 value to
PMU_REG7B"），不写的话 R0 的值撑不过复位。地址由
`include/soc/bk7258/reg_base.h:54` 的 `0x44000000` 加
`aon_pmu_ll.h` 里的偏移算出：R0=`0x44000000`、R25=`0x44000094`、
R7A=`0x440001E8`，都在 AON 域——这正是它能跨复位存活的原因。

**什么时候能碰**：不能在 `__start()` 里。AON 域没就绪就访问会挂总线，
第二章那两次砖机就是这么来的。所以锁存这一步放在 `board_late_initialize()`，
和 AON RTC 同一个位置，那里域已被证明可用。

`board_reset()` 现在会先写 `RESET_SOURCE_REBOOT` 再咬狗，好让下次启动
分得清"我自己要重启"和"看门狗自己咬的"。

**这一章的验证边界**：以上全是构建级取证（符号进镜像、`.config` 取值、
`romfs.img` 内容）。`board_reset_cause()` 的映射**没有上板验证**——尤其
"看门狗咬后 R7A 里是不是真的是 0x02"这一条，SDK 的 BK7236XX 分支并没有
对看门狗做特殊处理，只是原样读回软件上次写的值，所以**咬狗场景是否真能
报出 RWDT，必须真机复验**。另外 `board_reset()` 现在多了一次 AON PMU
写，而 `reboot` 是本仓的烧录入口——万一它出问题，退路是按 RST 键，
`bk_flash.py` 本来就不设超时（README 8.2）。

### 一个装不下的镜像，和为什么要拆配置

把整套必测配置加进 `configs/nsh` 之后，链接直接失败：

```
flash: 1806084 B / 1728 KB = 102.07%
```

`nsh` 是产品镜像，闭源 BLE 栈 + 显示管线已经吃掉 92%，塞不下 NIST STS
和 crypto 测试套件。硬塞的话只能砍产品功能，那是本末倒置。

拆成两个配置：

| 配置 | 用途 | flash |
| --- | --- | --- |
| `configs/nsh` | 产品镜像。轻量必测项：`/etc`、md5、BCH、复位原因 | 1652520 B / 93.4% |
| `configs/xts` | 验证镜像。全套必测 + C++；剥掉 BLE 栈与三个演示 app | 1262392 B / 71.3% |

官方流程本来就是这样——xTS 跑的是验证构建，不是出货镜像。跑完再把
`nsh` 烧回去。

拆配置时踩到的：`configs/xts` 是从 `nsh` 过滤蓝牙相关行生成的，结果把
`CONFIG_TLS_NELEM` / `CONFIG_TLS_TASK_NELEM` 一起滤掉了，而 `LIBCXX`
`depends on TLS_NELEM > 0`，libcxx 的 `thread.cpp` 还要 `task_tls_*`。
连着两轮链接失败才补回来。

### C++ 与 crypto 的两个非显性依赖

- `CONFIG_HAVE_CXX=y` **只是声明工具链会编 C++**，标准库是另一个 choice，
  默认 `LIBCXXNONE`。`cxxtest` 要 `<map>/<vector>/<fstream>`，得开
  `CONFIG_LIBCXX` + `CONFIG_LIBCXXABI`（两者都已 vendored 在
  `nuttx/libs/libxx/` 下，能离线编）。第一轮报的是 `fatal error: map:
  No such file or directory`。
- crypto 套件链接时报 `undefined reference to curve25519_generate_public`。
  根因在 `nuttx/crypto/CMakeLists.txt:61-63`：`curve25519.c` 被放在
  `if(CONFIG_CRYPTO_RANDOM_POOL)` 分支里，而 `cryptosoft.c` 的 DH 路径
  无条件引用它。所以 `CRYPTO_RANDOM_POOL` 在这里不是可选项。

### 没补上的三项，以及为什么

1. **1.3.16 RNG（nist_sts）**——`apps/testing/drivers/nist-sts` 不自带
   源码，configure 时从 csrc.nist.gov 下 `sts-2_1_2.zip`。本树这条流程
   是坏的：压缩包解出来叫 `sts-2.1.2`，而 CMakeLists glob 的是
   `sts/src/*.c`，一个源文件都找不到，于是 builtin 表里留下无法解析的
   `nist_sts_main`；两个随附补丁还有一个被拒（留下 `Oops.rej`）。修它要
   动 `apps/`，违反公共仓零改动。`/dev/urandom` 本身是在的，缺的只是
   NIST 那套统计工具。本地要跑的话在 gitignored 的下载目录里补个软链
   `ln -s sts-2.1.2 .../nist-sts/nist-sts/sts` 即可，但那不会跟着仓库走，
   所以没设成默认。
2. **1.3.13 Timer（`/dev/oneshot0`）**——用例要求 arch alarm 方案把
   oneshot 暴露成设备节点。AON RTC 只有两个硬件比较单元，`TICK` 给了
   系统时基、`UPPER` 给了 `/dev/rtc0` 闹钟（`bk7258_rtc.h:64-65`），
   没有第三路。把系统时基那一路直接注册成 `/dev/oneshot0` 会让测试程序
   抢走调度时钟，所以只能另起片内通用 TIMER 外设——新驱动，未做。
   `cmocka_driver_oneshot` 这个命令**在镜像里，但没有设备可开**。
3. **片内 flash MTD（1.3.5 的本意）**——`CONFIG_BCH` 已开，
   `cmocka_driver_block` 也在，拿 SD 卡的 `/dev/mmcsd0` 可以跑通块设备
   用例，但**片内 8MB flash 仍然没有驱动**。这块同时卡着 BLE 的出厂
   标定数据读取（十三章余量第 3 条）。难点是 flash 控制器 XIP 时的 CRC
   编码（34:32），MTD 要在这层之上还是之下要先想清楚。

## 十七、xTS 必测集真机跑通记：两个新缺陷，和一张被自己测试擦掉的卡

上一章把配置补齐了，本章是**真机执行**的记录。镜像用 `configs/xts`，
全部结论来自 `/dev/cu.usbserial-310` 上的实际输出。

### 通过的

| 用例 | 结果 |
| --- | --- |
| 1.3.5 块设备（真卡） | 3/3，耗时约 2 小时 |
| 1.1.1 内存管理 | 8/8 |
| 1.1.2 调度 | 16/16 |
| 1.1.3 系统调用 | 81 通过 / 2 失败（仅 socket，见下）|
| 1.1.4 ostest | `Exiting with status 0` |
| 1.1.8 hello | `Hello, World!!` |
| 1.1.10 popen、1.3.4 RAM 随机 | 3/3（ramdisk）|
| 1.3.10 UART | `cmocka_driver_uart` 1/1 |
| 2.1.4 Reboot 启动时间 | 7.5 秒到 NSH，启动日志零 error |
| 1.1.5 getprime | 1230 个素数 / 4765 ms |
| 1.1.6 mm | TEST COMPLETE |
| 1.1.7 scanftest | OK 164 / FAILED 0 |
| 1.1.9 helloxx | 静态/栈/动态三种构造 |
| 1.1.10 popen、1.1.11 pipe | 含重定向 |
| 1.1.12 md5 | 100 次一致，且与主机 `md5` 逐字节吻合 |
| 1.1.13 cxxtest | vector/string/map/C++17 |
| 1.3.2 fstest | OK 200 / FAILED 0 |
| 1.3.3 ramtest | 六种图案全过 |
| 1.3.12 RTC | 3/3，含 alarm 与 periodic 回调 |
| 1.3.17 crypto | **8/8** |

### 1.1.3 那 8 个失败，7 个是同一个原因

第一轮 75 通过 / 8 失败。其中 `sockettest01/02` 好定性——`socket()` 返回
EINVAL 而非期望的 EAFNOSUPPORT，因为没有 TCP/IP 栈，是 Wi-Fi 那座山的一部分。

剩下 `close03`/`fpathconf01`/`truncate01`/`write03`/`symlink01`/`symlink02`
一度查不出，报的都是 `fd > 0` 或 `ERROR: open test file fail`，**看着像文件
系统坏了**。我先排除了两个假设：CWD 不可写（框架自己会 `chdir` 到
`/tmp/<testdir>`，切过去重跑结果一样）、tmpfs 不支持 truncate（手工
`truncate -s 4` 把文件从 11 字节截到 4 字节，正常）。方向都不对。

**线索在 `write_test.c`**：三个测试用**完全相同**的
`open(filename, O_RDWR | O_CREAT, 0700)`，却只有 write03 失败。唯一差别是
文件名多了 `.wav` 四个字符。这些名字都是拿 `__func__` 拼出来的：

| 测试 | 文件名 | 长度 | 结果 |
| --- | --- | --- | --- |
| write01 | `test_nuttx_syscall_write01_file` | 31 | 通过 |
| close01 | `test_nuttx_syscall_close01_dir` | 30 | 通过 |
| close03 | `test_nuttx_syscall_close03_dir100` | **33** | 失败 |
| truncate01 | `test_nuttx_syscall_truncate01_file` | **34** | 失败 |
| write03 | `test_nuttx_syscall_write03_file.wav` | **35** | 失败 |
| fpathconf01 | `test_nuttx_syscall_fpathconf01_file` | **35** | 失败 |

**通过的全部 ≤32，失败的全部 >32，零例外** —— 而 `CONFIG_NAME_MAX` 是 NuttX
的默认值 32。上板实测钉死这个边界：32 字符的文件建得出来，33 字符的
**静默失败**，连文件都不落地，`echo x > 长名字` 在 NSH 里连报错都没有。

改成 `CONFIG_NAME_MAX=64`（树内其它板子用 48/64/96，抬这个值是常规做法），
镜像只大 72 字节。重跑：**通过 75 → 81，失败 8 → 2**，这四项连同
`symlink01/02` 一起转通过。

**教训**：一个报“打不开文件”的失败，未必和文件系统有关。这次能找到全靠
`write01`/`write03` 在同一个文件里、用同一段代码、只差四个字符——**天然
对照组比任何推理都值钱**，下次遇到"部分用例失败"先去找这种组。

上一章新加的三样东西在真机上都立住了：`/etc` ROMFS 挂载正确、`md5_test`
哈希与主机一致、libcxx 工具链跑通。

`reboot` 路径也顺带验证了：第二次烧录是在**含 `bk7258_reset_reason_set()`
的镜像**上用 `--reboot` 完成的，探测数 3340 与改动前的 3339 基本一致——
`board_reset()` 里新增的那次 AON PMU 写没有破坏重启。

### 缺陷一：复位原因报不出 RWDT（本章最重要的结论）

四次看门狗运行（`-r 0/1/2/3`）的全部失败收敛到同一处：

```
drivertest_watchdog.c:377   1 != 2
drivertest_watchdog.c:422   1 != 2
drivertest_watchdog.c:460   1 != 2
```

实际 `1 = SYS_CHIPPOR`，期望 `2 = SYS_RWDT`。feeding / interrupts / loop
三个子测试本身都通过，只有复位原因这一项挂。

**根因**：上一章从厂商 SDK 抄来的那套读写序列是对的，但它维护的字段
**纯靠软件写入**——`reset_reason_init()` 读完就写回 POWERON，硬件在
看门狗咬下去时不会往里写任何东西。所以咬狗后读到的永远是上次软件写的值。
这正是上一章末尾标注"必须真机复验"的那一条，**结论是证伪**。

**修法**（未做）：BK7258 有 NMI 看门狗阶段——`CONFIG_NMI_WDT_EN`、
`sys_hal_nmi_wdt_set_clk_div()`，见 `bk_idk/middleware/driver/wdt/wdt_driver.c:120`
与 `middleware/soc/bk7258/hal/wdt_ll.h:52`。咬狗前先进一次中断。官方用例
的注记恰好也要求这个："芯片厂商初始化 wdt 时需要在 wdt 中断里面主动调用
panic，并且提高 watchdog 中断优先级"。接上这一级同时解决两件事：ISR 里写
`RESET_SOURCE_WATCHDOG` 让下次启动报对，以及 panic + 堆栈转储。

动它要小心：看门狗同时是 `reboot` 和烧录窗口的入口。

### 缺陷二（**已撤回**）：块设备压测并没有把板子打死

**这一条是误判，后来推翻了。原始记录保留在下面，因为翻车过程本身比结论
有价值。**

当时的观察：`cmocka_driver_block -m /dev/mmcsd0` 在第一个子测试
`drivertest_block_stress` 之后串口再无任何输出，DTR/RTS 切换零响应，
只能人手按 RST。据此判定"卡死且看门狗没救回来"。

**三条推翻的证据**：

1. **后台重跑，889 秒全程 `Ready`。** 把同一条命令加 `&` 丢到后台，NSH
   就空出来了。`ps` 每 25 秒采样一次，任务始终是 `Ready`（可运行、在被
   调度），不是 `Waiting Semaphore`；栈高水位稳定在 1268~1328 字节；
   NSH 全程响应；板子没有重启过。**它一直在正常干活。**
2. **前台被动重跑，1501 秒无异常。** 不发任何命令、只被动读串口，跑满
   25 分钟窗口仍在正常运行——没有卡死，没有重启，也没有任何输出（该测试
   循环里本来就不打印）。
3. **两次都远超当初判定的 300 秒。**
4. **最终它自己跑完了，而且全过。** 让它在真卡上不受干扰地跑到底，约
   **2 小时**后打出：

   ```
   [       OK ] drivertest_block_stress
   [       OK ] drivertest_block_single_write
   [       OK ] drivertest_block_cache_write
   [  PASSED  ] 3 test(s).
   ```

   日志尾部还能看到一小时前发的 `echo alive` 排在队里、测试一结束就执行了
   ——**NSH 全程只是被前台任务阻塞，从来没死过**。

**根因是我读错了现象**：`cmocka_driver_block` 是 NSH **前台任务**，它运行
期间 NSH 本来就不回显；而这个测试在循环里一个字也不打印。于是"发命令没
回显"被我当成了"系统死了"。至于 DTR/RTS 零响应——README 8.2 早就写明本板
CEN 没接到 CH340 控制线，**那本来就永远无效**，拿它当死机佐证是循环论证。

**看门狗"没救回来"同样是误判**：系统健康、tick 正常、狗被正常喂着，
不咬才是对的。这里没有需要救的东西。

**这个测试到底要跑多久**：12 MB ramdisk 跑 23347 次迭代用了 465 秒
（约 50 次/秒）；SD 卡是 233472 次迭代还要走真实 I/O，实测 889 秒内前沿
连扇区 15000 都没到，推算全程**数小时**。等 300 秒就下结论，等于在一场
四小时的长跑第五分钟宣布选手猝死。

**教训一**：判断"卡死"之前先确认自己有没有观察通道。把长任务丢后台、留出
一个能敲命令的 shell，`ps` 一眼就能分清"Ready 在跑"和"Waiting 卡住"——
这个动作成本几乎为零，却能省掉一整条错误的排查链（我为此写了 ramdisk 隔离、
读了三层驱动的等待路径、还擦了一次卡）。

**教训二**：别拿恒为真的条件当证据。当时我把"DTR/RTS 切换零响应"也算作
死机佐证，可 README 8.2 早就写明本板 CEN 没接到 CH340 控制线，**它永远
无效**。用一个永真命题去支持结论，是循环论证。

**顺手补掉的洞：控制台 Ctrl-C**。此前跑飞的前台任务完全没法中断，只能等它
跑完或按 RST——这正是把上面那次误判的代价放大的原因之一。

先走了一段弯路。两个 defconfig 加了 `CONFIG_SIG_DEFAULT=y` +
`CONFIG_TTY_SIGINT=y`（`CONFIG_SIG_SIGKILL_ACTION` 默认就是 y），构建通过、
`.config` 确认生效，我就宣布"补上了"——**真机上拿 192 秒的 `ostest` 连测两次，
两次都杀不掉**。`.config` 里有值 ≠ 功能能用，这跟本章开头那次误判同源。

然后又读了一轮代码，逐环节看都是对的：`uart_register()` 会为控制台置
`ISIG | ECHO | ICANON`（serial.c:2102）；NSH 前台执行前发 `TIOCSCTTY`
注册 pid、结束后发 `TIOCNOTTY` 释放（nsh_builtin.c:151/216），两个 ioctl
驱动里都实现了；`INVALID_PROCESS_ID` 是 -1，首次注册不会被挡；信号投递就在
`uart_recvchars()` 末尾的 `nxsig_tgkill()`，而且非 DMA 路径的调用点在
`serial_io.c:288`，我们这条路是通的。读到这里就卡住了——**每一环都对，但它
就是不工作**。

**转折是停下来写探针**（`src/sigtest.c`）。两个门槛其实都能从用户态问出来：
`tcgetattr()` 直接给 `c_lflag`；`TIOCSCTTY` 的返回值直接反映 pid 槽状态
（返回 EINVAL 反而是健康的——说明已经有人注册了）。两个数一出来答案唯一：

```
c_lflag = 0x00000000   ISIG=NO ICANON=no ECHO=no
TIOCSCTTY -> -1, errno 22   (pid 槽已占用，这一环正常)
```

**ISIG、ECHO、ICANON 三个全零**。这三个是 `uart_register()` 里同一条语句设的，
全零就意味着那条语句从没执行——即注册时 `dev->isconsole` 为假。而设它的
`arm_earlyserialinit()` **在本移植里从来没有被调用过**：早期控制台是走
`bk7258_lowputc()` 自己起的，`__start` 不调它，NuttX 的 arch 代码里也没有
调用点。于是 `uart_check_special()` 第一句 `if ((tc_lflag & ISIG) == 0)`
直接返回，Ctrl-C 那个字节只是缓冲区里普通的一字节。

**修法一行**，在 `arm_serialinit()` 注册控制台之后：

```c
CONSOLE_DEV.tc_lflag |= ISIG;
```

**刻意不去设 `isconsole`**——那会连带打开 ECHO 和 ICANON，而 NSH 的 readline
自己做行编辑与回显，驱动层再来一遍就是双重回显；本章第一节那场控制台 RX
的仗也说明这块行为不该顺手动。只置真正需要的那一位，修完
`c_lflag = 0x1`，ECHO/ICANON 仍为 0。

真机复验：`ostest` 正常需 192 秒，**8.5 秒被 Ctrl-C 杀掉**，shell 立刻收回。

**教训**：读代码读到"每一环都对但就是不工作"时，那一步该停——继续读只会
把同一个盲区再走一遍。改成向系统提问（哪怕只是两个能从用户态读到的数），
一次就定死了。`sigtest` 留在树里，下次控制台信号出问题不必再推一遍。

**排查过程留档**（结论虽然作废，路径本身可复用）：

| 实验 | 迭代数 | 耗时 | 结果 |
| --- | --- | --- | --- |
| ramdisk 512 KB（`mkrd`） | 972 | < 120 秒 | 通过 |
| ramdisk 12 MB | 23347 | 465 秒 | 通过 |
| 真卡高位扇区单扇区读写（主机驱动 `dd`） | 800 次传输 | 243 秒 | 通过 |
| 真卡后台跑压测 | —— | 889 秒 | **不是卡死，一直 Ready** |

顺带确认了几件事，都成立：`bk7258_sdio.c` 的四个等待循环全部有
`POLL_BUDGET = 2000000` 保护，无无界等待；`mmcsd_transferready()` 被
`TICK_PER_SEC` 界住；bch 层七处 `nxmutex_lock` 都是规整配对；块设备按路径
`open()` 走 `fs_blockproxy.c` → `bchdev_register()`，`dd` 和测试走的是同
一条路。**这一层没有已知缺陷。**

### 缺陷三（自伤）：那个测试是破坏性的，卡被擦了

`drivertest_block_stress` 的写法是：

```c
nsectors = pre->cfg.geo_nsectors * SECTORS_RANGE;   /* SECTORS_RANGE 0.95 */
for (i = 0; i < nsectors; i++) { lseek; 写随机; fsync; lseek; 读回; crc32 比对; }
```

它对整卡 95% 的扇区逐个写随机数据。跑了约 300 秒才卡死，事后取证：
扇区 0 是随机 ASCII、`mount -t vfat` 返回 EINVAL、偏移 1MB/10MB/100MB
采样全是随机数据。**原厂表情素材被擦掉了**，而 `bk7258_backup/` 里只有
flash 备份，没有卡的镜像。

**教训**：xTS 里带 "stress" 字样的用例要先读源码确认它写什么设备。
官方文档只说"在测试平台 /dev 下找到 Flash 对应的设备名称"，没有一个字
提到它是破坏性的。跑之前先备份目标盘，或者拿一张空卡。

### 灌回素材：贴片卡只能走串口

SD NAND 是贴片的（SDIO 走 GPIO14-19），拔不下来用读卡器，只能从控制台灌。
给 `configs/nsh` 加了 `CONFIG_SYSTEM_YMODEM`（+10.5 KB，加完 93.99%，
刚好装下），顺带补上了此前没跑的 xTS 1.3.11（Uart 文件传输）。

三个必须踩准的细节：

1. **`CONFIG_FAT_LFN is not set`**，NuttX 这边只认 8.3 大写短名。face 应用
   待机时开的是 `/mnt/GENIE_~1.AVI`——那是原厂用支持长名的系统写入
   `genie_eye.avi` 后生成的短名。灌回去必须直接叫 `GENIE_~1.AVI`。
2. **lrzsz 的 `lsz` 在 macOS 上驱不动 `/dev/cu.*`**。`lsz ... < 口 > 口`
   会把设备开两次；改成共享 fd（`exec 3<>口`）仍然在收到接收端第一个
   `C` 之后挂住。最后自己用 pyserial 写了个 Ymodem-1K 发送端
   （`scratchpad/ysend.py`），一个句柄、可控重试。
3. **启动 `rb` 和发送必须在同一个进程里**。分两个进程时，接收端的第一个
   `C` 正好落在关口/开口的缝里被吃掉，发送端永远等不到。

实测速率 8.6 KB/s（115200 下理论上限 11.5 KB/s 的 75%，Ymodem 停等 ACK
加 SD 写入的开销），9.1 MB 约 18 分钟。没有去调高控制台波特率——UART
分频万一在高速下不准会把控制台一起丢，那要重新烧录才能救。

**结果**：10 个文件全部灌回，板上 `ls -l /mnt` 的大小与主机暂存逐字节一致，
再用板上的 `md5_test`（就是上一章为 xTS 1.1.12 加的那个）逐个复算，
**10 个哈希与主机 `md5` 全部吻合**。`face` 应用起来后正常进入 idle 循环，
无任何文件打开失败。卡从 120 MB 空盘变为已用 9360 KB。

顺带一提，这次校验本身就是 `/etc` + `md5_test` 那套工作的意外回报——
补 xTS 用例时顺手做的工具，成了灌数据后唯一能在板上做端到端验证的手段。

### 仍然跑不了的三项

- **1.3.6 GPIO**：用例要 `cmocka_driver_gpio -a /dev/gpio0 -b /dev/gpio1`
  两个节点用杜邦线短接，板上只注册了 `gpio0`。
- **1.3.7 I2C/SPI**：用例走 uORB + BMI160 传感器，板上没有。
- **1.3.16 RNG**：打包问题已解决，但卡在第二道坎上。

  **第一道（已解决）**：`apps/testing/drivers/nist-sts` 不自带源码，configure
  时从 NIST 下载，然后两处对不上——压缩包解出来叫 `sts-2.1.2` 而它自己的
  CMakeLists glob 的是 `sts/src/*.c`；`PATCH_COMMAND` 用
  `patch -p0 -d <dir>/nist-sts`，而补丁里的路径以 `nist-sts/sts/` 开头，
  **`-d` 深了一级**，两个补丁全被拒（留下 `Oops.rej`）。两者叠加的结果是
  一个源文件都找不到，**构建照样成功**，只在最后链接时炸出未解析的
  `nist_sts_main`。

  修法写成了 `tools/fix_nist_sts.sh`：把解出的目录改名成 `sts`，再用
  `-p2` 应用两个补丁。全部动作发生在该包 `.gitignore` 排除的下载目录内，
  **不改任何公共仓的跟踪文件**。改完链接通过、`nist_sts` 进 builtin 表、
  交互菜单跑得起来。fresh checkout 后要重跑一次这个脚本。

  **第二道（未解决）**：套件要同时为 15 个测试各开 `stats.txt` 和
  `results.txt`，加上 summary、freq.txt 和输入文件，超过 30 个流。实测在
  **第 11 个日志文件**上 `fopen` 返回 NULL，套件报
  "LOG FILES COULD NOT BE OPENED / MAX # OF OPENED FILES HAS BEEN REACHED = 11"
  ——那条消息还带一句 "-OR- THE OUTPUT DIRECTORY DOES NOT EXIST"，容易误导，
  但目录确实都在（前 6 个测试的日志文件已经建出来了）。

  已排除的：15 个目录名与源码 `testNames[]` 逐字一致；把
  `CONFIG_NFILE_DESCRIPTORS_PER_BLOCK` 从 8 抬到 64 后**仍然停在 11**，
  所以不是 fd 表大小；`CONFIG_LIBC_OPEN_MAX` 是 256。改到 FAT 上跑更早就挂
  （`CONFIG_FAT_LFN` 没开，`AlgorithmTesting` 超出 8.3），那条路不通。

  **第二道也查清了。** 写了 `src/fdtest.c`（xts 镜像里的 `fdtest` 命令），
  把两个池子分开测，一次就定死了：

  ```
  open()   held  40 simultaneously (无失败)
  fopen()  held  13 simultaneously, then failed with errno 24 (EMFILE)
  -> FILE streams run out first (13 vs 40 descriptors)
  ```

  裸描述符开到 40 毫无压力，FILE 流卡在 13。所以我一开始拧
  `NFILE_DESCRIPTORS_PER_BLOCK` 是拧错了旋钮——那管的是描述符表。

  根因在 `nuttx/libs/libc/stdio/lib_fopen.c:94`：

  ```c
  if (list->sl_count >= _POSIX_STREAM_MAX) { set_errno(EMFILE); return NULL; }
  ```

  而 `_POSIX_STREAM_MAX` 是 `nuttx/include/limits.h:131` 里**硬编码的 16，
  没有任何 Kconfig**。减去 stdin/stdout/stderr 正好 13，与实测分毫不差。

  **这不是板级问题**，任何 openvela 板子都一样：NIST 套件要同时开 30+ 个流
  （15 个测试各 2 个日志 + summary + freq + 输入文件），在这棵树上全量跑
  不可能。官方却把它列为必测——值得往上游报。

  **绕过办法：分批跑。** 日志额度是 10 个流 = 每轮 5 个测试。在
  `nist_sts` 的测试选择那一步答 `0`（不全选），再输入 15 位的位串。
  三批实测结果（判据 P-Value > 0.0001，比例及格线 8/10）：

  | 测试 | P-VALUE | 比例 |
  | --- | --- | --- |
  | Frequency | 0.739918 | 10/10 |
  | BlockFrequency | 0.534146 | 10/10 |
  | CumulativeSums | 0.739918 / 0.122325 | 10/10 |
  | Runs | 0.534146 | 10/10 |
  | LongestRun | 0.066881 | 10/10 |
  | Rank | 0.534146 | 10/10 |
  | FFT | 0.122325 | 10/10 |
  | OverlappingTemplate | 0.008878 | 10/10 |
  | Universal | 0.534146 | 10/10 |
  | ApproximateEntropy | 0.534146 | 10/10 |
  | Serial | 0.035173 / 0.534146 | 10/10 |
  | LinearComplexity | 0.739918 | 9/10 |

  **15 项全部产出结果且全部达标**，最低的 0.008878 仍高出判据近两个数量级，
  比例最低 9/10 也高于及格线 8。**`/dev/urandom` 的均匀性与独立性达标。**

  分批用的位串（测试选择那步答 `0` 之后输入）：

  ```
  111110000000000   Frequency BlockFrequency CumulativeSums Runs LongestRun
  000001101100000   Rank FFT OverlappingTemplate Universal
  000000000010011   ApproximateEntropy Serial LinearComplexity
  ```

  **另外 3 项一度跑不了，后来查清并解决了两件事。**

  症状：NonOverlappingTemplate（148 个模板）、RandomExcursions（8 个状态）、
  RandomExcursionsVariant（18 个状态）都在报告生成阶段停在
  **`data12.txt` -- file not found**，`finalAnalysisReport.txt` 留空。

  **不是"必须同时打开"**。`partitionResultFile()`（`src/assess.c:120`）本来
  就是"开一批 → 写 → 关一批"，作者早就避免了一次开 148 个：

  ```c
  m = numOfFiles/20;                                  /* 分成 m 批 */
  for (k=0; k<m; k++) {
      for (i=start; i<=end; i++) fp[i] = fopen(s[i], "a");   /* 开一批 */
      ...写...
      for (i=start; i<=end; i++) fclose(fp[i]);              /* 关一批 */
  }
  ```

  问题只是**批大小硬编码 20**，而此处可用额度是 11（16 减去 stdin/stdout/
  stderr、summary、results.txt）——所以第 12 个必挂，编号严丝合缝。
  `fix_nist_sts.sh` 里把这四个字面量改成 8，三个测试的 `dataN.txt` 立刻
  全部生成。

  **NonOverlappingTemplate 还缺一份输入**。改完批大小后它跑完了，但 148 行
  全是 `0.000000 / 0/10 / *`。根因在
  `src/nonOverlappingTemplateMatchings.c:41`：

  ```c
  sprintf(directory, "templates/template%d", m);      /* 相对当前目录 */
  ```

  那是 NIST 发行包 `templates/` 下的文件（`template9`，2664 字节，正好
  148 行模板），不随测试代码走。板上工作目录没有它，测试读不到模板就产出
  一堆零——**看着像随机数不合格，其实是缺输入**。

  解法：把它烘进 `/etc` ROMFS（`src/etc/templates/template9`，走的是
  `/etc/1.txt` 那套现成机制，镜像只大 3 KB），跑之前拷到工作目录：

  ```
  mkdir -p /tmp/templates
  cp /etc/templates/template9 /tmp/templates/template9
  ```

  补上之后：**148 行，129 项 10/10、19 项 9/10，零个 `*` 标记，全部达标**。

  **RandomExcursions 与 Variant：我先后给出过两个错误解释，实际是我的
  测试方法有 bug，这两项本来就是通过的。**

  它俩的 `stats.txt` / `results.txt` 一直是空的。我先说是"流上限"，
  后说是"循环数不足、样本不适用"——**两个都错**，而且第二个是从空输出
  倒推出来的，没有任何直接证据。

  真因：`fixParameters()` 只在选了**参数化测试**（BlockFrequency、
  NonOverlapping、Overlapping、ApEn、Serial、LinearComplexity）时才弹参数
  调整菜单。RandomExcursions 和 Variant 都不是，所以那一步不出现——而我的
  自动化脚本不管选了什么都照发同一串 7 个输入，多出来的那个 `0` 被下一个
  提示 "How many bitstreams?" 吃掉，**比特流数变成 0**，测试循环一次都没
  执行。文件当然是空的，而 "Statistical Testing Complete" 照样打印。

  去掉多余那一步重跑，立刻正常：

  ```
  (a) Number Of Cycles (J) = 1833
  (b) Sequence Length (n)  = 400000
  (c) Rejection Constraint = 500.000000
  SUCCESS    x = -4 chi^2 = 3.826565 p_value = 0.574647
  ... 8 个状态全部 SUCCESS
  ```

  **J = 1833，是门槛 500 的三倍多**——样本一直是够的，我那句"典型只有约
  316 个循环"是凭空估的，跟实测差了近 6 倍。

  汇总报告：RandomExcursions 8 行、RandomExcursionsVariant 18 行，
  **全部 7/7，零个 `*` 标记**。P-VALUE 列显示 `----` 是因为有效序列数
  （7）不足 10，STS 此时跳过均匀性卡方、只报通过比例；报告结尾那句
  "with the exception of the random excursion (variant) test" 说的就是
  这两项用不同的判定口径。10 条序列里有 3 条因循环数不足被逐条丢弃——
  **"不适用"是按序列判的，不是整个样本不合格**。

  **教训**：自动化交互式程序时，输入序列不能写死——菜单是条件出现的。
  更要紧的是，**不要从"输出是空的"去反推原因**。空输出的成因太多，我两次
  都在没有证据的情况下编了一个听起来合理的机制，第二次还写进了文档。

  **一个每次都要记得的操作**：`/tmp` 是 tmpfs，**重烧镜像后那 15 个
  `experiments/AlgorithmTesting/<测试名>` 目录全没了**，跑之前必须重建，
  否则套件会报 "Could not open freq file"，看着像别的问题。

## 十八、NMI 看门狗阶段：让复位原因说真话，顺带换来 capture

十七章留下的唯一功能缺陷是复位原因报不出 `SYS_RWDT`。根因当时已经查清——
AON PMU 那个字段纯靠软件维护，硬件咬狗时不写。本章记录把 BK7258 的 NMI
看门狗阶段接上，让"咬狗"这件事有一个能执行代码的时刻。

### 这颗芯片有两个看门狗，不是一个

`wdt_hal.c` 里 `wdt_hal_close_unused()` 把它们当作互斥的两选一，但那是厂商
的策略，不是硬件约束。两块可以同时跑：

| 块 | 地址 | 行为 | 本仓用途 |
| --- | --- | --- | --- |
| AON_WDT | `0x44000600` | 直接复位芯片 | 死机兜底 + `reboot`/烧录入口 |
| NMI_WDT | `0x44800000` | 抬 NMI 异常 | 先咬一口，留出记录和 dump 的时间 |

`wdt_hal_init()` 在 `CONFIG_SOC_BK7236XX`（BK7258 属于这一族）且
`CONFIG_NMI_WDT_EN=y` 时选的是 `NMI_WDT_ID`——也就是说**厂商自己的默认就是
NMI 那块**，本仓此前只用了 AON 那块。

寄存器布局（`wdt_struct.h`）：`+0x08` 是 global_ctrl，bit1 旁路时钟门控；
`+0x10` 是 ctrl，低 16 位周期、[23:16] 密钥，和 AON 块同样的
`0x5A` 解锁 / `0xA5` 提交双写。

### 计数率不是猜的

周期单位是这一章唯一需要外部依据的量。厂商 `bk7258.defconfig` 写
`CONFIG_INT_WDT_PERIOD_MS=8000`，而 `wdt_ll_set_period()` 在时钟分频为 /16
（`bk_wdt_driver_init()` 里 `NMI_WDT_CLK_DIV_16` 程的值）时把毫秒数乘 2。
两者合起来：周期寄存器 16000 ↔ 8000 ms，**即 2 kHz**，0xffff 上限约 32.8 秒。

这条很重要:心跳是 100 ms 一次,周期若真是 26MHz/16 那一档,0xfffc 只有
40 ms,一上电就会陷入 panic 循环。正常周期取 8 s——比心跳宽 80 倍,又远短于
AON 块的 ~65 s,所以 NMI 永远是先咬的那个。

### 时钟必须先开,否则又是一块砖

`bk7258_wdt_arm()` 里那段注释记着:第一版曾照 bootloader 同时写
`0x44800010`,放在 `__start()` 顶部,板子直接砖成全无输出——未上电的 APB 块
被访问会挂总线。所以本章的 init 有严格顺序:

1. `0x44010030 |= 1<<31` 打开 WDG_CPU 设备时钟(该寄存器 audio/pwm 已在用,
   地址是验证过的;bit31 来自 `CLK_PWR_ID_WDG_CPU` 在厂商枚举里的位置)
2. `0x44800008 |= 1<<1` 旁路块内时钟门控
3. `irq_attach(NVIC_IRQ_NMI, ...)`
4. 才允许写 `0x44800010`

整个初始化放在 `board_late_initialize()`,和 AON RTC、复位原因锁存同一处。
`g_nmi_wdt_live` 这个标志确保在此之前任何 `bk7258_nmi_wdt_arm()` 都是空操作。

### 真机验证

| 现象 | 证据 |
| --- | --- |
| NMI 真的来自 NMI | `xPSR: 68000002`,低 9 位 = 异常号 2 |
| **关中断也能打进来** | `BASEPRI: 00000080` 的那次 dump——正是官方注记要求的"打断 critical_section" |
| 咬狗前能打堆栈 | `Assertion failed panic: at bk7258_wdt.c:198`,完整寄存器+双栈转储 |
| 复位原因报对了 | `-r 1/2/3` 的 RWDT 断言全部通过(改动前是 `1 != 2`,四次运行全挂) |
| 静置无误触发 | 烧录后静置 30 秒零输出 |

`cmocka_driver_watchdog -r 3` **四个子测试全部 PASSED**;`-r 0`/`-r 1`/`-r 2`
按官方要求的顺序各跑一遍,**零断言失败**(每轮都以设计中的那次咬狗结束)。

### 顺手拿到的 capture

`-r 3` 里 `drivertest_watchdog_api` 还要 `WDIOC_CAPTURE`——咬狗前回调。
下半部此前 `.capture = NULL`,注释写的是"AON 块直接复位、没有可挂钩的时刻,
承诺回调就是撒谎"。那句话在 NMI 阶段出现之后不再成立:NMI 就是那个时刻。

语义是**回调替代复位**:装了 handler 就在 NMI 里喂回两块狗再调用它,系统继续
跑;没装才走记录原因 + panic 的老路。

### 为什么不让 panic 自己复位

`CONFIG_BOARD_RESET_ON_ASSERT` 默认 0,assert 不复位。这里刻意保持:
让 AON 块在 dump 之后咬下去。走 `board_reset()` 会把复位原因写成 REBOOT,
把刚记录的 WATCHDOG 覆盖掉——那样复位原因又会说谎,只是换了个谎。
NMI 处理里把 AON 周期重设为 `BK7258_WDT_PERIOD_DUMP`(5000,约 5 秒),
既够 115200 上打完 dump,又不至于让板子悬着。

### 边界

- **`-r 0` 已验证**(按 RST 后补跑)。它要求起始复位原因是 `SYS_CHIPPOR`,
  也就是上一次必须是上电或按键复位;经 `--reboot` 烧录后拿到的是 `CORE_SOFT`
  ——这本身是对的,那次确实是软件重启,所以要按一次 RST 才满足前置条件。
  按 RST 后 `-r 0` 零断言失败,喂狗循环跑完、停喂、NMI 咬下(`xPSR` 低 9 位
  = 2),板子复位;接着 `-r 1` 同样零失败。**官方要求的 `-r 0→1→2→3` 全序列
  按顺序跑通。**
- **capture 回调跑在 NMI 上下文**,在调度器所有锁之外。测试里的回调 `sem_post`
  是能用的,但这不是一个可以放任意代码的地方。
- 2 kHz 这个数是从厂商配置推出来的,不是示波器实测。8 秒周期的实际时长没有
  精确计时过——只验证了"静置 30 秒不误触发"和"停喂后约 100 ms 内咬"。

## 十九、Timer 驱动：一次靠探针而不是靠读代码定位的时钟

必测 1.3.13 要一个 `/dev/oneshot0`。此前的记录把它列为"补不上"，理由是 AON RTC
两个比较单元都已占用。这个理由本身没错，但结论下早了——BK7258 片内还有两组
TIMER（`0x44810000` / `0x45800000`，中断 3 / 13），本仓的 `bk7258_memorymap.h`
和 `chip/include/irq.h` 里早就定义好了，缺的只是驱动。不需要任何额外硬件。

### 为什么必须另起一路，而不是复用 RTC 那个 oneshot

`bk7258_rtc.c` 里已经有一个 `bk7258_oneshot_initialize()`。链接时撞名才让这件事
变得显眼：它被 `bk7258_timerisr.c` 直接喂给 `up_alarm_set_lowerhalf()`，**它就是
系统 tick**。把同一个下半部再注册成 `/dev/oneshot0`，等于把调度器的时钟交给测试
去反复 arm/cancel。TIMER 组 0 是独立的第三路，正是为了让用例随便压而碰不到调度器。
两者已同轮验证互不干扰（`cmocka_driver_oneshot` 与 `cmocka_driver_rtc` 全通过）。

### 为什么占两个通道

用例的判定方式决定的：

```c
ioctl(fd, OSIOC_START, &delay);
ioctl(fd, OSIOC_CURRENT, &before);
sigwaitinfo(...);
ioctl(fd, OSIOC_CURRENT, &after);
assert_in_range((after - before).tv_sec, delay.tv_sec - 1, delay.tv_sec);
```

它拿**前后两次 `CURRENT` 之差**去核对延时。所以 `current` 必须是一条跨 arm、
到期、cancel 都单调自由运行的时间线——一个每次到期就归零重装的通道做不到。
于是 ch0 按需装填做 oneshot，ch1 满量程（`0xffffffff`）自由跑做时基，溢出由
中断累加高 32 位。26 MHz 下 32 位约 165 秒绕一圈。

### 接口有两套，选错了编译都过不去

`nuttx/include/nuttx/timers/oneshot.h` 里 `oneshot_operations_s` 有两个版本，由
`CONFIG_ONESHOT_COUNT` 切换：老的收发 `timespec`，新的（clkcnt）只收发原始计数、
换算全在上半部。本仓**已经是 clkcnt 那套**——`BK7258_RTC` 的 Kconfig 里有一句
`select ONESHOT_COUNT if ALARM_ARCH` 把它带了进来。第一版按 timespec 写的，查
Kconfig 时才发现，没等上板就返工了。新驱动 `select ONESHOT_COUNT`，把这个组合
变成构建期约束而不是注释里的一句提醒。

`start_absolute` 故意不实现：通道是从 0 数到终值，没有"自由时间线上某个绝对时刻"
的概念；上半部的兜底路径会关中断、减去 `current()`、再按相对延时装填，那正是这个
硬件能做的。

### 真正的坑：计数时钟要写 `soft_reset` 才启动

第一次上板，`max_delay` 报得完全正确（165.191049807 s = `0xffffffff`/26e6），
之后 `OSIOC_START` 就再也不返回。**但 `max_delay` 是纯软件常量，它正确不能证明
硬件在跑。**

写了 `src/timertest.c` 直接读外设窗口（flat 构建，用户态可以读），把三种成因分开：
时钟没到模块 / 通道没在计数 / 计数到了但中断没送达。第一轮结果：

```
SYS  clk_en   90f88195  (bit4 timer0 = on)        门控开着
SYS  clk_sel  44100031  (bit20 tim0  = 26M xtal)  源选对了
SYS  int_en0  00000018  (bit3 irq3   = routed)    中断路由通了
TIMER ctrl    00000002  en=010 div=0              通道使能了
TIMER end1    ffffffff                            终值装好了
chan1  0 -> 0  delta 0   (expect ~2600000)        就是不走
```

**这里走了一段弯路，记下来。** 看到 `TIMER global_ctrl` 的 `clk_gate_bypass`
为 0，而 NMI 看门狗当初正是卡在同名的位上，就直接认定是它、改了、烧了。结果
`global` 读回 `00000002`、位确实写进去了，计数**照样是 0**。假设错了。

更要紧的是，这一轮暴露了**探针自己的盲区**：读计数要走一个握手（写通道号 →
写触发位 → 等硬件清触发位 → 读保持寄存器），我的轮询超时后**静默地**去读寄存器，
拿到 0——和"计数器停着"完全无法区分。补上握手是否超时的报告后，真相立刻出来：

```
rd chan1: after_trig=00000005  HANDSHAKE TIMED OUT (1000 spins)  value=00000000
TIMER dev_id  54494d52     <- ASCII "TIMR"
```

块是活的（`dev_id` 读出 `"TIMR"`，写进去的值都存得住），但那个握手**是由定时器
自己的时钟域完成的**——它永不完成，说明功能时钟根本没到，而不是计数器停着。

于是不再一个假设烧一轮，让探针一次跑四个实验：

| 实验 | 100 ms 增量 | 结论 |
| --- | --- | --- |
| E1 写 `soft_reset` 后重启通道 | **2,782,510** | ✅ 就是它 |
| E2 关掉 `clk_gate_bypass` | 2,813,031 | 照常跑，**这位根本不需要** |
| E3 时钟源切 32K | 3,461（理论 3277） | 源选择位语义正确 |
| E4 组 1 未做 soft_reset | 0，握手超时 | 同一机理，非组 0 特有 |

**`global_ctrl` 的 `soft_reset` 是这个块计数时钟的启动开关，只开系统控制器那边的
门控不够。** 它不自清，且置 1 就是工作态（不是"保持复位"）——名字有误导性，但
E1/E4 两侧对照无歧义，也与厂商 `timer_ll_init()` 结尾无条件写一次、永不清除的做法
一致。E2 同时否掉了 `clk_gate_bypass`，那一位已从驱动删掉：厂商的定时器路径压根
不碰它。

漏读的原因也记一下：查 `timer_ll_init()` 时用了 `head -20`，恰好把结尾那行
`timer_ll_soft_reset()` 截掉了，于是得出"厂商不动 `global_ctrl`"的错误结论。

### 频率是交叉验算过的，不是照抄常量

E1 测到 2,782,510，比 26 MHz × 100 ms = 2,600,000 高约 7%。这不是常数错了，是
`usleep(100000)` 会超时。用 E3 的已知 32768 Hz 源反推：3,461 / 32768 = 105.6 ms，
把这个实际时长套回 E1 得 **26.3 MHz**。如果 26 MHz 这个常数写错了，32K 那一组
不会同样偏高——两组一起偏高才说明偏差在计时而非在频率。

### 三条会静默失败的规则

改这个文件之前先看文件头注释，三条都不是能从寄存器名猜出来的：

1. **计数时钟靠 `soft_reset` 启动**（上面那段）。
2. **没有独立中断使能位。** SDK 叫 `timerN_int_en` 的那位是 pending 状态，读回即
   状态、写 1 清除——通道一跑就必然中断。`timer_ll_set_interrupt()` 和
   `timer_ll_set_chan_interrupt_status()` 写的是同一个 bitfield，逐字核对过。
3. **清中断必须自旋到读回为 0。** 该位在 26 MHz 域，写完要几个周期才落地，立刻
   读还是 1；不自旋处理函数会立即重入。SDK 也是这么自旋的。

另外 `ctrl` 寄存器的低 7 位（三个通道使能 + 分频）是普通状态，位 7-9 是 W1C 状态，
所以每次读改写都要把高三位掩掉，否则会顺手确认掉别的通道的中断。驱动里所有对
`ctrl` 的写都过 `TIMER_CTRL_KEEP` 这一个口子。

### 真机结论

`cmocka_driver_oneshot -d /dev/oneshot0` **连跑 3 次全通过**；同轮 `cmocka_driver_rtc`
（api / alarm / periodic）与 `cmocka_posix_timer` 全通过，确认没动到系统 tick 和
AON RTC。

### 边界

- **165 秒的溢出回绕没有被测试覆盖。** 用例最长跑约 30 秒，高 32 位累加那条路径
  （含"wrap 已发生但中断还没跑"时读 pending 位补偿的那段）是照着竞态推导写的，
  **未经真机验证**。要验证得让板子空跑 3 分钟以上再读 `CURRENT`。
- **最短延时被抬到 64 个计数**（约 2.5 µs）。装填终值和置使能是两条相隔几个总线
  周期的写，终值太小会在通道启动时就已经落在计数器后面，那一枪要等整整一圈
  165 秒才到。抬高是保守取值，没有实测过真正的下限在哪。
- **只用了组 0。** 组 1（`0x45800000`，中断 13）三个通道和组 0 的 ch2 都还空着。
- `timertest` 留在树里。下一次这个块出问题，应该是几分钟的事而不是几轮烧录。

## 二十、片内 flash MTD，以及它撞出来的时基竞态

必测 1.3.5 的块设备用例此前是拿 SD 卡代跑的，片内 flash 一直没有驱动。本章记录
把它补上的过程，以及补的过程中撞出来的一个**不在 flash 驱动里、而在系统时基里**
的既有缺陷——后者花掉的时间是前者的好几倍，值得完整留档。

### 驱动本身：四条实测事实

固件在这块 flash 上 XIP 运行，写错一次就没有串口了（README 第八章记过一次），
所以先只读探测（`src/flashtest.c`），把决定驱动形状的问题一个个测掉。四条里有
三条推翻了看起来最自然的默认做法：

1. **控制器在 `0x44030000`，`dev_id` 读出 ASCII `"FLSH"`。** 厂商树里有两套寄存器
   映射，`middleware/driver/flash/flash.c` 那套被 `CONFIG_SOC_BK7256XX` 包着、
   指向 `0x00803000`，而本芯片属于 `BK7236XX` 族——照着它写会全盘落空。真正的
   映射在 `soc/flash_struct.h`：`op_cmd` 在偏移 **0x54**（地址 + 操作码），
   `op_ctrl` 在 0x10（触发位 29 / 忙位 31）。偏移用 `REG_0xd + sec_addr[4]×2 =
   REG_0x15` 独立验算过。

2. **软件通路是物理地址、原始字节。** 控制器给 CPU 取指路径每 32 字节插 2 字节
   CRC，所以 XIP 地址是物理地址的 32/34——但这个映射**不适用于软件通路**。判据是
   读物理 `0x11020` 得到 `4fc8...`，与 `nuttx_crc.bin` 同偏移逐字节一致，其中
   `4fc8` 正是第一个 32 字节块的两个 CRC 字节；解码视图不可能给出这个结果。所以
   MTD 完全不涉及 CRC。

3. **芯片出厂全片写保护。** bootloader 把 BP 位（SR[6:2]）留在 `0x1f`，而 NuttX
   里没有厂商 flash 驱动去管它。不解保护，编程会被 flash **静默丢弃**，读回来像
   驱动是死的。每次擦写前清、事后还原（实测写后 `SR` 回到 `0x27c`）。

4. **区间不是随便选的。** 厂商分区表累加正好 `0x400000`（68k+1836k+476k+136k+
   1428K+144K+4K+4K），而本板是 8MB，上半部无人认领。但扫描发现
   `0x400000..0x4d0000` 是**被 CRC 编码过的空白**（`000c` 经主机验算正是 32 个 FF
   的 CRC），而且**最后 6 个扇区在用**，`0x7fe000` 开头是 `"TLV"`——出厂校准数据，
   这块板 BLE 能用很可能就靠它。所以区间取 `0x500000..0x7F0000`（2.94 MB），
   下边界清开编码区留余量，上边界给出厂数据留整 64 KB。驱动对**每一条**擦写路径
   做区间校验，越界直接 `-EFAULT`。

其它实测数字：一次读-改-擦-写 4 KB 扇区 **80–92 ms**；擦写是 32 字节为单位，
**先填 8 个字再下命令**（与读相反）；擦写路径不显式发 WREN，控制器自带。

### 真机结论

`cmocka_driver_block -m /dev/mtdblock0` 三个用例（stress / single_write /
cache_write）**全过**，多轮复现。此外 FAT 格式化 / 挂载 / 写文件 / `cat` 读回、
`df` 报 2986K 全部正常；数据落在正确物理地址由**独立的原始寄存器通路**交叉验证
（写入 `i*7+0x5a` 序列，读物理 `0x500000` 得到 `5a61686f767d848b...`）。

### 撞出来的东西：时基会在重 flash 负载后死亡

压测**通过之后**，板子会在 IDLE 咬狗复位。这个现象花了八轮才定位，前七轮全错，
错误清单和推翻它们的证据见本章末——那部分比结论更值得读。

真凶是 **AON RTC 比较寄存器的跨时钟域写入竞态**，在 `bk7258_rtc_arm()`：

```c
now   = bk7258_rtc_ticks();
delta = target > now ? target - now : 1;   /* 目标已过 → 退到 1 tick */
putreg32((uint32_t)(now + step), CMP);
```

比较器靠**相等**匹配一个自由运行的 32 kHz 计数器。而 AON 在常开域，从 CPU 侧写
它**本身就要最多 31 µs 才生效**，与「最小 1 tick」正好同量级——比较值可能在写入
落地前就被计数器越过，此后**永不匹配**。而唯一能重装该通道的又是它自己的中断，
于是系统时钟永久死亡。

控制台仍能回显，是因为 UART 走自己的中断，不依赖定时器。所以现象是**板子答得动
却没有时钟**：`sleep` 永不返回，喂狗随之停止，恰好一个看门狗周期后咬下。

**修法是把余量做进值里**（最小 4 tick = 125 µs），不是写完回读验证——那一版试过，
无效，因为回读那一刻新值还在路上。

> 同一条规则本仓已经踩过第二次了：TIMER 驱动里 W1C 状态位必须**自旋到读回为 0**，
> 因为跨 26 MHz 域要几个周期（十九章）。AON 慢三个数量级，而且**没有可回读的
> 东西**，所以只能靠余量。跨时钟域的外设，写完不等于生效。

**归属要说清楚：这个 bug 在时基里，一直存在，不是 flash 驱动引入的。** 它只是第一个
能把 31 µs 窗口撑开的负载（每写 32 字节屏蔽中断自旋约 0.7 ms，是窗口的 20 倍）。
围着 flash 驱动查的那五轮，方向从一开始就错了。

### 顺带修掉的两个真 bug（各自独立成立）

查这条线时回头核对厂商的看门狗代码，发现两处：

- **周期算错 8 倍。** 我把计数率硬编码成 2000 Hz，注释还写得理直气壮："厂商编程的
  分频是 /16，所以块以 2 kHz 计数"。而厂商 `wdt_ll_set_period()` **每次都从硬件读
  分频器**（`ckdiv_wdt`，系统块 reg 0x0a 的 bits[3:2]），再按 16/8/4/2 换算。实测
  本板 `clkdiv=0` → **16 kHz**。也就是说那个自称 8 秒的 NMI 看门狗，从接上那天起
  **一直是 1 秒的**。
- **超限用掩码而非钳位。** period 字段 16 位，8000 ms × 16 = 128000 溢出，被
  `&= 0xffff` 悄悄截成 62464。把「要求过长」变成「实际很短」，是最危险的失败方式。
  厂商用钳位。

改后周期取 4000 ms——在这个分频下 16 位字段的物理上限是 4095 ms。启动日志现在会打
`wdt: nmi clkdiv=0 -> 16 counts/ms, period 64000 = 4000 ms`，让这件事在每次启动
时都可见。

**注意这两个修复都不是那个崩溃的根因**，改完照崩。它们只是各自独立成立的真缺陷。

### 八轮排查：错的七次，和推翻它们的证据

留档的目的不是自省，是省下这七条死路。共同毛病只有一个：**先给一个合理的机制，
再拿「现象消失」当验证**，而没有取到能区分因果的证据。

| # | 假设 | 推翻它的实测 |
| --- | --- | --- |
| 1 | 擦除期间 CPU 停摆取不到指令 | 计时循环在擦除间隙正常执行并打印 |
| 2 | 加喂狗修好了 `mkfatfs` 复位 | 那次观察**根本没抓转储**，论据是空的 |
| 3 | 魔数序列被中断打断导致装填丢失 | 加 `up_irq_save()` 后照崩 |
| 4 | flash 活动饿死 tick / 喂狗路径 | `clock()` 与 `CLOCK_MONOTONIC` 全程吻合在 10 ms 内 |
| 5 | 在 NMI 处理程序里 `_alert()` 打印现场 | NMI 在 PRIMASK 之外，syslog 取锁会死锁；**连原本能拿到的转储都没了**（已撤销） |
| 6 | 看门狗周期算错 8 倍是根因 | 窗口从 1 秒放宽到 4 秒，照咬 |
| 7 | 比较寄存器写完回读验证即可 | 无效——回读那一刻新值还在路上 |

真正推动进展的四步全是**取证**而非推理：

- `addr2line` 解出完整回溯，证明是真咬、并查清连崩是 AON 短周期跨复位存活所致
- `ostest`（600 秒重负载、不碰 flash）**零断言零重启**，把问题锁进 flash 相关路径
- 给捕获脚本加时间戳，测出咬狗**恰好在测试结束后一个完整周期**——这个数字排除了
  「断续饿死」，只剩「喂狗完全停止」
- **`sleep` 判定**：它不碰看门狗，直接判「时基死活」。此前所有推理都在用受怀疑的
  子系统去验证关于它自己的结论，环环相扣却每环都没独立验证过

第 5 条尤其值得记：**探针本身也会有 bug，而且会给出看似成立的结论。** 第一版
`sleep` 判定报「时基存活」，其实匹配到的是命令回显；`flashtest` 第一版的握手超时
是静默的，与「计数器停着」无法区分；`grep` 只截输出尾部，把最有价值的转储开头
丢了。判据没有区分力时，取证和猜想一样不可靠。

### 边界

- **`configs/nsh` 未启用片内 flash MTD。** 产品镜像已到 95.16%，而这条路径对人脸
  demo 没有用途。时基与看门狗那两处修复对两个配置都生效。
- **区间以外从未写过一个字节。** 区间校验覆盖所有擦写入口，出厂 `"TLV"` 数据和
  三个核的镜像都在校验之外。
- **擦除耗时 80–92 ms**，调度时要按这个量级预算。至于 flash 忙时 CPU 究竟还能不能
  取指，**没有测过**——早期一个版本的注释断言它不能，那是无据之词，并且把整场排查
  引偏过一次。
- `flashtest` 留在树里：读控制器状态、指定地址转储、区间扫描、MTD 自检、复位原因、
  擦写计时。下次这块出问题，应该是几分钟而不是八轮。

## 二十一、WiFi：把厂商 MAC 接进来，以及一次代价高昂的误判

这一章要先撤回它自己的旧标题。

### 撤回：「卡在两个没发布的头文件上」是错的

旧结论是：`components/bk_wifi/src/*.c` 缺 `sm_task.h` 和 `ps.h`，三份 SDK 全盘 `find` 都没有，所以公开 SDK 自己都编不过 WiFi 组件，只能向厂商索要。

**这是错的。** 那两个 `#include` 都写在

```c
#if NX_VERSION > NX_VERSION_PACK(6, 22, 0, 0)
#include "sm_task.h"
#include "ps.h"
#endif
```

里面，而本树的 `NX_VERSION` 是 6.8.2.0，预处理器根本走不到那两行。`components/bk_wifi/src` 的 **33 个源文件按发布原样全部编过**，一个头都不用要。

错因值得记下来，因为它在这一轮里以三种形式各犯了一次：

1. **grep 到文本就下结论，没看它被什么条件守着**——上面这条。
2. **把 `//` 注释掉的 include 当成有效依赖**统计。
3. **`2>/dev/null` 把「目录不存在」变成空输出**，空输出又被读成「文件不存在」。

代价是：一整章错误的勘察结论、一次 813 MB 的官方 SDK 下载、一次无效的三方对比，以及差点就要向厂商发出的索要请求。把它拦下来的是两次追问——先是「你有好好找了吗 一般这里都有的」（我只改了措辞，没重新验证），然后是「这些项目 bk_idk 里的项目都是可以编译的，那它们缺少头怎么编译的？」——第二问逼出了实际验证。**用户对事实的直觉比我的 grep 可靠。**

同一批被推翻的还有「`bk_idk` 是声网定制裁剪版」：`git remote` 显示 `bekencorp/bk_idk`，两份 SDK 都是官方版。

### 当前状态

`configs/xts` 打开 `CONFIG_BK7258_WIFI` + `CONFIG_BK7258_WIFI_VENDOR` 后，闭源 MAC 会在板上真正执行，控制台打出它自己的：

```
IP Rev: 802.11ax
mm_bcn_loss_info: wait 0,wake 0,intv 0,prevent 5,loss_thd 200
```

随后在 `mm_init` 内部硬故障。**尚不能收发。**

### 三个「做错也不报错」的接缝

这三个都不是编译错误、不是链接错误，全部表现为几十层之后闭源代码里的取指故障。

**一、`bk_wifi_init()` 带 config 参数。**
真实签名是 `bk_err_t bk_wifi_init(const wifi_init_config_t *config)`。本仓一度手写 `extern int bk_wifi_init(void)` —— 能编、能链，调用时 r0 是调用方残留的垃圾。厂商第一句就是专防此事的 `if (config->os_funcs == NULL) return BK_FAIL;`，**而垃圾不是 NULL**，检查通过，垃圾指针被存进 `g_wifi_funcs`，`rwnxl_init()` 再从它偏移 0x238 取函数指针跳过去 → `CFSR=0x00000100`（IBUSERR），现场在闭源库里，没有任何未定义符号或链接告警。

纪律：**厂商 API 一律用它自己的头**。`chip/bk7258_wifi_glue.c` 存在的唯一理由就是这个——它用 vendor flags 编译，因此能 include `modules/wifi_types.h` 并使用厂商自己的 `WIFI_DEFAULT_INIT_CONFIG()` 宏。

**二、适配表会被 `--gc-sections` 回收。**
`g_wifi_os_funcs` 是 211 项函数指针表，但**没有任何代码按名字引用它**——`nm --undefined-only` 扫全部 32 个闭源库，一个引用都没有。于是 `-ffunction-sections -fdata-sections --gc-sections` 把它整个收走，`sta_mgmt_init` 之流从 `.bss` 里的 NULL 指针取表。

`WIFI_DEFAULT_INIT_CONFIG()` 里的 `.os_funcs = &g_wifi_os_funcs` 是它唯一的存活理由。

**表活过来之后，未定义符号从 44 涨到 99。那不是倒退**，是之前被 GC 静默隐藏的真实集成面终于可见。最终收敛到 0。

**三、PHY/RF 适配器必须先注册。**
`libbk_phy.a` 的 `rf_open_handler`（`rf_cntrl.c:188`）从 `.bss` 的 `g_rf_funcs_t` 取表、按偏移 16 跳转。填表的是 `phy_adapter_init(&g_phy_os_funcs, &g_phy_os_variable)`，厂商在 `bk_init.c` 的 `app_phy_init()` 里调，本仓不走那条路。

**这两张表本仓早就有**——`chip/bk7258_phy_osi.c`，BLE 移植时写的，文件头注释就写着「必须在任何人碰射频之前跑，否则库里每个指针都是 NULL」。但只有 `bk7258_ble.c` 调初始化，而 xts 配置剥掉了 BLE。现已在 `bk7258_wifi_ifup()` 里带幂等保护地调用。

不要改成编译厂商的 `components/bk_phy/src/bk_phy_adapter.c`：那会和 `bk7258_phy_osi.c` 撞 `g_phy_os_funcs` 等四个符号。

### 两个极性陷阱

- **电源岛 `0x44010040`（`cpu_power_sleep_wakeup`）的位是「掉电位」**。`sys_hal_module_power_ctrl()` 开电源是**清位**。写反了就是把正在跑的域关掉。
- **电源子模块不是域**。`PM_POWER_SUB_MODULE_NAME_PHY_WIFI = 201`，不是一个位；厂商用 `父域 × PM_MODULE_SUB_POWER_DOMAIN_MAX(20) + 序号` 生成，所以 `201 / 20 = 10` 才是 PHY 域号。用除法推导，别列区间表——区间表在厂商插入新子模块时会静默过时。

时钟门控 `0x44010030` 的编号规则可以交叉验证：`PM_CLK_ID_WDG_CPU = 31`，正是 NMI 看门狗那条「先开 `0x44010030` bit31」（十八章）用的同一位。

### 已排除的假设

这一轮推翻的自己的判断，逐条记下来，免得下次重走：

| 假设 | 判据 | 结论 |
|---|---|---|
| 栈溢出 | `CFSR=0x01000000` 我按 STKOF 读，实际 bit24 是 **UNALIGNED**（STKOF 是 bit20）；三次启动实测栈只用 **960 字节** | **错**。据此把 `CONFIG_INIT_STACKSIZE` 提到 32768 的改动已回退 |
| 发生了厂商断言 | 栈上的 `dbg_wifi_assert_handler` 地址是 **Thumb 函数指针值**（某处注册的回调），不是返回地址；且全程无断言输出 | **错**。是裸故障 |
| 堆未清零导致链表头是垃圾 | `me_strategy_mem_init()` 四次 `malloc` 确实无 memset，厂商堆在 `.bss` 里天然全零——但在 `up_allocate_heap()` 里清零后真机**毫无变化** | **错**，已回退 |
| 内存不足 | 给 `os_malloc` 加失败日志，一次都没触发；堆约 300 KB | **错** |
| `sta_info_tab` 是 NULL | 实测 `0x28055628`，8 字节对齐 | **错** |
| `CONFIG_WIFI_MAC_SUPPORT_STAS_MAX_NUM` 与闭源库不一致（旧章标的 ABI 风险） | 实测 `g_wifi_mac_sta_max_num = 2`，与 `me_strategy_mem_init()` 的 `2*568+1136` 分配式一致；值来自 `sdkconfig.h`，在 `-I` 首条上 | **不成立**，不需要手工对齐 |

**方法论教训**：`R8=0x280444e8`/`R10=0x238` 这类「故障寄存器指向某函数」的推断，在 `arm_hardfault → _assert` 这条路径上不可靠；栈上的地址也可能是函数指针值而非返回地址。**判据必须能独立复现**，否则就是在给自己造证据。

### 当前卡点与下一步

故障被夹在两个确定位置之间：`mm_env_init()`（打印 `mm_bcn_loss_info` 的那个，是 `mm_init` 的第 2 个调用）之后，其余九个调用之一里 —— `vif_mgmt_init` / `sta_mgmt_init` / `td_init` / `ps_init` / `txl_cntrl_init` / `rxl_init` / `mm_timer_init` / `scan_init` / `chan_init`。

拿不到真正的故障 PC 是最大障碍：NuttX 把 BusFault/UsageFault 留在关闭状态，它们升级成 HardFault，`_assert` 打印的是它自己的上下文，而保存的寄存器组落在栈转储范围之外。

**已准备好但尚未跑的办法**：接管异常向量。`irq_attach()` 对系统异常号同样有效（4=MemManage，5=BusFault，6=UsageFault），处理函数的 `context` 参数就是保存的寄存器组，`regs[REG_PC]` 即故障指令地址；配合 `NVIC_SYSHCON`（`0xE000ED24`）的 bit17/bit18 使能，异常就不会升级。实现见 `bk7258_wifi_shim.c` 的 `bk7258_wifi_fault_probe_install()`，已链入镜像，等板子接回来即可烧录取证。

### 规模事实（仍然有效）

- 闭源 `libwifi.a` 外部依赖：本轮实际解决 **99 个符号**，全部在本仓实现或由厂商开源文件补齐，**没有改动任何厂商源码**。
- `libwifi.a` **一个 `pbuf_*` 都不引用**，所以报文缓冲布局不属于闭源 ABI。但厂商开源侧通过 `-I` 看到的是 **lwIP 2.1.2 的 `struct pbuf`**（`rwnx_rx.c:16` 的 `#include "pbuf.h"` 是无守卫的），所以 `bk7258_wifi_pbuf.c` 必须编在 vendor OBJECT 库里、看同一份头——该结构体在 `PBUF_LIFETIME_DBG` 下会长出尾部字段，布局不一致不会报错，只会静默错位。
- 厂商自带的 `components/bk_wifi/src/pbuf.c` 用不了：整个文件被 `#if (CONFIG_FULLY_HOSTED || CONFIG_SEMI_HOSTED)` 挡着，而且它写 `p->total_len` 而全树其余代码读 `p->tot_len`，是旧变体的残留。
- supplicant（`wpa_supplicant-2.10`，150 个 .c / 171.6k 行）尚未移植。scan/monitor 不经过它，所以缺它的代价是**不能关联**，不是没有射频。相关桩全部会在首次被调用时打一行日志。

### 文件分工

| 文件 | 编译方式 | 职责 |
|---|---|---|
| `bk7258_wifi.c` | NuttX flags | netdev 下半部，注册 `wlan0`，`ifup()` 里拉起厂商栈 |
| `bk7258_wifi_shim.c` | NuttX flags | 平台服务，按 **REAL / ADEQUATE / PENDING** 三类分节；PENDING 每个首次调用打一行日志 |
| `bk7258_wifi_osi.c` | NuttX flags | RTOS 原语（线程/队列/信号量/定时器/字符串/临界区） |
| `bk7258_wifi_pbuf.c` | **vendor flags** | pbuf 分配器 + `ethernetif_input` 桥接 |
| `bk7258_wifi_glue.c` | **vendor flags** | 需要厂商头才能取对参数类型的调用 |
| `bk7258_wifi_vendor.cmake` | — | 33 个厂商源 + `bk_feature.c` + 我们那两个，113 个 `-I`、16 个 `-D` |

PENDING 那一类的一行日志不是装饰。第一次上板时，`wifi: power vote not ported` 正好出现在硬故障的上一行，直接指出电源岛没开——**静默的桩和能工作的实现在出事之前完全无法区分**。

## 二十二、host 链接崩溃结案：被换库顺手修好，却没人回去复测

十三章第八战把「链接闭源 host 就射频 BusFault」记成悬案，「发射未拿下」
和「经典蓝牙做不了」两条结论都挂在它上面。2026-08-29 复查：**坑早就没了**，
是被一次为别的原因做的换库顺手修掉的，只是没人回去测。本章记结案过程，
以及它顺带暴露的一个**现存构建回归**。

### 一、时间线，精确到分钟

| 时刻 | 提交 | 做了什么 |
|---|---|---|
| 22:39:39 | `d40447c` | 放弃 host 实验（**崩溃在此观测**） |
| 23:17:45 | `0979dda` | 把 `libbk_phy.a` 换成 `libcom_phy.a`（为的是第五战那个空槽位） |
| 23:48:10 | `1ae021c` | 写下第八战 |

崩溃现场在 `calibration_main → bk7011_cal_tx → bk7011_cal_rx_adc_dlym`
——**PHY 校准代码里，不是 host 代码里**，第八战自己也写明 host 一次都没被
调用。而 38 分钟后被撤掉的 `libbk_phy.a`，正是这段校准代码当时的提供方；
撤它的理由（`nv_init` 跳空槽位 INVSTATE）也落在同一条调用链上。
两个崩溃同源的可能性很高，但文档写在换库之后、复测在换库之前，
**中间那 38 分钟把因果关系割断了**。

### 二、复测（真机，`/dev/cu.usbserial-10`）

把当年的探针加回去（`bk7258_ble_link_probe()` 里取 `bluetooth_host_init`
的地址），在今天的树上构建：

| | 基线 | host 强制链入 |
|---|---|---|
| host 符号进镜像 | 2 | **369** |
| `.text` | 1654829 | 1749301（**+92 KB**） |
| `.bss` | 147072 | 152640（+5568） |
| `ld.script` 的 SRAM 越界 ASSERT | — | **未触发** |
| `face BT 6` 扫描 | — | **2055 / 1972 条，`RADIO WORKS`** |

两轮扫描都干净，`uptime` 连续无复位，`free` 16.5 MB。**没有 BusFault。**
+92 KB 与 `d40447c` 提交信息里那句「95K of host archive」吻合，说明抽取
规模复现无误，崩溃却不复现。

### 三、顺带纠正一处机制描述

第八战说「把 host 库链接进来」。**归档在链接线上不等于代码进镜像**：
`libbluetooth_host_ble.a` 有 618 个导出符号，常态下只有 **2 个**进镜像
（`gapc_env`、`gapc_get_conidx`）。原因是 `libbluetooth_controller_ble.a`
的 `hci.c.obj` 引用了 `gapc_get_conidx`，链接器为解析它只抽出一个
`gapc.c.obj`，该对象其余函数连同它们的未定义引用一起被 `--gc-sections`
丢掉。真正把 92 KB 拉进来的是**取 host 函数地址**这个动作——和第二战
「让探针函数取一下地址才算数」是同一个机制的两面。

顺带排掉两个曾经的嫌疑：`libbk_phy.a` 与 `libcom_phy.a` **导出符号完全
相同**（各 512 个，零差集），选谁纯粹由链接顺序决定，差别只在实现；
host 与 controller 之间**零重名、零弱符号、零 common**，静默覆盖不成立。

### 四、副产品：`nsh` 配置从 2026-08-07 起就编不出来

复现的第一次构建直接链接失败，只有一个未定义符号：

```
bk7258_phy_osi.c:983: undefined reference to `bk_phy_set_nv_reg_hook'
```

用**未经改动的干净 HEAD 树**验证，同样失败——**这不是实验引入的，
是现存回归**。`5ad4798`（2026-08-07，WiFi 那一轮）给
`phy_osi_nv_reg_set_hook()` 加了对 `bk_phy_set_nv_reg_hook()` 的调用，
注释写「already linked」。那句话对 `configs/xts` 成立、对 `configs/nsh`
不成立：该符号在 `libwifi.a` 里，而 `libwifi.a` 由
`CONFIG_BK7258_WIFI` 门控，产品配置根本不链它。

同文件 `bk7258_phy_adapter_init()` 里的另一处调用**早就有**
`#ifdef CONFIG_BK7258_WIFI_VENDOR` 保护，983 行漏了。已按同样方式补上。

两条纪律：

1. **「already linked」这类断言要按配置分别核实。** 本仓有两个配置，
   一个链 WiFi 归档一个不链，凡是跨归档调用都要问「哪个配置」。
2. **产品配置要有人定期构建。** 这个回归活了三周没被发现，因为期间
   所有工作都在 `configs/xts` 上做。

### 五、经典蓝牙：一条走到底才发现前提是错的岔路

> **结论先行：BK7258 是 BLE-only，本节记的是一次误判和它的取证过程。**
> 用户查了 BK7258 数据手册，上面只有 BLE、没有 BR/EDR。以下"芯片支持
> 经典蓝牙"的推断**作废**，保留过程是因为错法本身值得记。

第八战结案时顺带推翻了「经典蓝牙做不了」，依据是三条**间接**证据：

- `bk_libs/bk7258/libs` 里躺着 `libbluetooth_controller_bt.a`、
  `libbluetooth_controller_dual.a`、`libbluetooth_host_dm_bt.a`；
- `bk_avdk_smp/projects/bluetooth/headset` 有 A2DP sink + HFP 的工程，
  且带 `ap/config/bk7258_ap/` 与 `cp/config/bk7258/` 两个配置目录；
- `libbluetooth_controller_dual.a` 里有 18 个 `LM_*`（经典链路管理）符号。

**三条都只证明"归档里有这些代码"，一条都不证明"这颗 die 上有那个模块"。**
数据手册是直接证据，它们不是。事后再看，SDK 里本来就摆着一条反向线索：
`bk_idk/components/bk_bluetooth/soc/` 下**只有 `bk7236` 一个目录**——整个
蓝牙组件是围着 bk7236 组织的，本仓的 BT OSI 表当初也正是从
`soc/bk7236/bluetooth.c` 复刻的。`bk_libs/bk7258/libs` 更像家族共用的构建
产物目录，不是按 die 裁剪过的清单。

### 五点五、那条岔路上真机测出来的两件事

判断错了，但路上做的实测有效，都留档。

**换双模归档本身是成立的**（`libbluetooth_controller_dual.a` 换掉
`_ble.a`，与现用同代、OSI ABI 不变）：

- 链接缺口正好 3 个，与静态分析一致：`ble_ethermind_thread_handle`、
  `ht_tid`、`wt_tid`。三个都是 4 字节 BSS，由 DM host 的三个对象提供，
  而控制器的 `ble_main.c` **并不调用**它们，只把地址登记进
  `.data.g_ble_task_info`（5 项 × 12 字节的任务栈配置表），所以在本仓
  自行定义清零变量即可，不必链 `libbluetooth_host_dm.a`。
  （注：先前算出的"5 个符号"多算了两个——`get_ble_txpwr_table_size`
  在已链的 `libcom_phy.a` 里，`strncmp` 是 libc。算缺口时忘了减掉
  已链归档的定义。）
- 镜像里 `LM_*` 从 0 变成 18，flash 45.10% → 50.26%（+193 KB），
  sram 67.32% → 68.07%。
- **BLE 无回归**：真机扫描 1900 条广播，`RADIO WORKS`。

**顺带挖出一个与经典蓝牙无关的既有缺陷**：本移植的 VHCI 路径对
`HCI_Reset`(0x0c03)、`Read_Local_Version`(0x1001)、
`Read_Local_Supported_Features`(0x1003)、`Read_Buffer_Size`(0x1005)、
`LE_Read_Local_Features`(0x2003) **一律拿不到应答**，控制器侧打

```
assert_param, param0 = 4353, param1 = <opcode>, file = hci_send_2_host, line = 1109
```

而同一颗控制器上 `LE_Set_Scan_Enable`(0x200c) 应答正常、扫描照跑。
**双模与 BLE-only 两个归档表现完全相同**（五个 opcode、param 值逐一
一致），所以这不是换归档带来的，是既有限制。以后想用 HCI 问控制器
任何事情，都会先撞上它。根因未查。

代码实验已全部撤回（Kconfig 开关、CMakeLists 门控、三个符号、HCI 探测
命令），树上只留文档与那一行 `#ifdef` 修复。

### 六、本章的验证边界

- 证明的是**链接通过 + host 在场时射频正常**。厂商 host 的代码路径
  一次都没被调用过，所以「host 能用」**没有测**，只测了「host 在场
  不再毁射频」。
- **BR/EDR 一次都没有真机证据**，正反两个方向都没有：读控制器特征位的
  路走不通（第五点五节那个 `hci_send_2_host` 断言），所以本章从未测出
  「支持」，也从未测出「不支持」。判定 BLE-only 的依据是数据手册，
  不是本章的任何一次实验。
- 板子已刷回修复后的产品镜像并复验（扫描 2132 条）。树里只留那一行
  `#ifdef` 修复，探针实验已全部回退。

### 七、教训

**「定位到但没解决」的悬案，要记下它是在哪个构建配置上测的。** 第八战
把现象、排除项、调用链都记得很完整，唯独没记「当时链的是 `libbk_phy.a`」
——而这正是 38 分钟后被改掉的那个变量。悬案的价值取决于它能不能被复现，
能不能复现取决于当时的配置有没有被记下来。

**归档里有代码，不等于硅片上有那个模块。** 第五节那次误判，三条依据全是
「SDK 里存在某某 `.a` / 某某工程配置」。厂商的 `bk_libs/<chip>/libs` 是家族
共用的产物目录，不是按 die 裁剪的能力清单。**芯片能力问数据手册**，这本来
就是本仓「硬件事实要有出处」那条纪律的原意，只是过去一直用它约束寄存器
和引脚，没想到芯片级能力同样适用。

**每个实验都要有对照。** `hci_send_2_host` 那个断言，第一眼像是双模归档
引入的回归；把 BLE-only 归档烧回去跑同一条探测，五个 opcode 的断言和参数
逐一相同——**是既有限制，不是回归**。少这一步就会往 PORTING_NOTES 里写进
一条假的因果。
