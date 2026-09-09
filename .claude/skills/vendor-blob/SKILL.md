---
name: vendor-blob
description: 把厂商闭源预编译库（.a）接进 NuttX/openvela。触发词：闭源库、厂商库、predefined、.a 归档、OSI 适配表、undefined reference、链接失败、符号缺失、gc-sections、WiFi 栈、BLE 控制器、blob。
---

# 闭源厂商库集成

WiFi MAC、BLE 控制器、射频 PHY 这类库只有 `.a`，没有源码。移植的本质**不是
逆向，是重新填表**——厂商把库与 OS 的全部耦合收敛在几张函数指针表里，那是
给"换 OS"预留的接缝。

本 Skill 是这条路上会静默失败的地方。**静默**是关键词：下面每一条做错都不报错。

## 一、先量接缝，再动手

动手前用 `nm` 把工作量算出来，别凭感觉：

```bash
NM=<toolchain>/arm-none-eabi-nm

# 归档需要什么（未定义符号）
$NM --undefined-only lib厂商.a | awk 'NF==2{print $2}' | sort -u > /tmp/need.txt

# 归档自己能解决多少（内部引用）
$NM --defined-only lib厂商.a | awk 'NF==3 && $2~/^[TDBRWV]$/{print $3}' | sort -u > /tmp/have.txt

# 真正要我们补的
comm -23 /tmp/need.txt /tmp/have.txt
```

⚠️ **算缺口时别忘了减掉其它已链归档和 libc 的定义**，否则会高估。
> 曾算出"要补 5 个符号"，实际只有 3 个——`get_ble_txpwr_table_size` 在已链的
> `libcom_phy.a` 里，`strncmp` 是 libc。

**同代归档换型号通常很便宜**：BLE-only 换双模，实测缺口只有 3 个符号，
而且都是 4 字节 BSS 变量（厂商 host 的线程句柄，只被登记进一张表、从不调用）。

## 二、`--gc-sections` 会吃掉你的适配表

**没有任何代码按名字引用适配表，闭源库也不引用**（它按内存布局读）。所以
链接器认为它是垃圾，整个对象连同它对闭源符号的全部重定位一起丢掉——
**第一次"构建成功"其实什么都没链上**。

两种保活方式：

```c
/* 1. 让探针函数取一下地址 */
uintptr_t xxx_link_probe(void)
{
  return (uintptr_t)我的_osi_init + (uintptr_t)我的_phy_init + ...;
}

/* 2. 或者靠厂商的初始化宏引用它 */
WIFI_DEFAULT_INIT_CONFIG()   /* 宏体里引用 g_wifi_os_funcs，这是它唯一的存活理由 */
```

> 211 项 WiFi 适配表活过来之后，未定义符号从 44 涨到 99。**那不是倒退，
> 是真实集成面终于可见。**

反过来也要知道：**归档在链接线上 ≠ 代码进镜像**。`libbluetooth_host_ble.a`
有 618 个导出符号，常态下只有 2 个进镜像——链接器只抽出解析得上的那一个对象。

## 三、厂商 API 一律用它自己的头，别手写原型

手写 `extern int bk_wifi_init(void);` 能编能链。但厂商真实签名带 config 参数，
那句专防此事的 `config->os_funcs == NULL` 检查会被**寄存器残留值**躲过，
故障出现在三十层之后的闭源代码里。

**凡是厂商 API，`#include` 它的头。**

## 四、ABI 握手要当真

好的厂商库会检查表的版本号和结构体尺寸。握手通过说明**逐字节布局正确**，
这是你能拿到的最强验证信号。

复刻结构体时：**字段顺序和类型一个都不能动**，即使某些字段你填 NULL。

## 五、验证布局：反汇编比头文件可信

不要对着头文件核对偏移，**反汇编闭源库自己的代码**：

> `rf_module_vote_ctrl` 取偏移 `0x104` 调用日志函数，而链接后的镜像里
> `g_phy_os_funcs + 0x104` 正好是我们的 `phy_osi_log`。**二进制自证。**

## 六、配置抄一半比全不抄更危险

厂商源码里某个槽位在"关 Wi-Fi"配置下填 NULL，照抄**没错**；但厂商关 Wi-Fi 时
链接的是另一个 PHY 归档，它的 `nv_init` 只有一条 `bx lr`，根本不读那一格。
而我们**两个库都链**，链接顺序里 Wi-Fi 版胜出，于是去调了一个"关 Wi-Fi 配置
从来不填"的空钩子。

**配置抄对了一半，另一半在链接命令行里。** 换库时要连带检查所有"因为那个库
不读所以填 NULL"的假设。

## 七、跨配置的链接假设要分别核实

同一份代码在不同 config 下链的归档不同。注释写"already linked"时，
**问一句：哪个配置？**

> 产品配置 `nsh` 曾**三周编不出来**：某处调用的符号在 `libwifi.a` 里，
> 而只有 WiFi 配置链那个归档。注释写着 "already linked"——对 xts 成立，
> 对 nsh 不成立。同文件另一处调用一直有 `#ifdef` 保护，那一处漏了。

**纪律：产品配置要有人定期构建。** 这个回归活了三周没被发现，因为期间所有
工作都在验证配置上做。

## 八、内存布局

闭源库的 `.bss` 可能很大（BLE 控制器 37 KB）。链接脚本里
`IDLE 栈顶 = _ebss + CONFIG_IDLETHREAD_STACKSIZE` 是**纯算术表达式，
链接器从不做区间检查**——`.bss` 一涨就把栈顶顶出 SRAM 区界，
`up_allocate_heap()` 算出负数回绕成 ~4 GB，分配器当场瓦解，**控制台一个字
都没有**。

加一条链接期 `ASSERT`，让下次撞它变成构建错误而不是砖：

```ld
ASSERT(_ebss + CONFIG_IDLETHREAD_STACKSIZE <= ORIGIN(sram) + LENGTH(sram),
       "IDLE stack top past end of SRAM")
```

## 九、路径不要写死

厂商 SDK 的位置因人而异。环境变量 + 同级目录回退：

```cmake
set(BK_IDK_ROOT "$ENV{BK_IDK_ROOT}")
if(NOT BK_IDK_ROOT)
  get_filename_component(BK_IDK_ROOT "${NUTTX_DIR}/../../bk_idk" ABSOLUTE)
endif()
```

写死绝对路径 = 别人 clone 下来编不了，而且会把开发机用户名提交进公开仓。

## 分类标注你的实现

适配表里几十上百项，必须让读者一眼看出哪些是真的：

```c
/* REAL     —— 真实现，行为与厂商等价
 * ADEQUATE —— 简化实现，当前路径够用，注明简化了什么
 * PENDING  —— 桩，首次被调用时打一行日志
 */
```

`PENDING` 那一行日志不是装饰。第一次上板时 `wifi: power vote not ported`
正好出现在硬故障的上一行，直接指出电源岛没开——**静默的桩和能工作的实现，
在出事之前完全无法区分。**
