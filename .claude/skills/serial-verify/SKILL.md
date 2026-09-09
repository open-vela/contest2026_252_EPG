---
name: serial-verify
description: 通过串口驱动 NSH 做真机验证——发命令、收输出、判断板子死活。触发词：串口、真机验证、看 log、板子卡住了、没回显、nsh 不响应、跑一下命令看看、验证一下、死机了。
---

# 串口真机验证

板子的唯一观察通道是 UART0（**115200 8N1，无流控**）。这个 Skill 是关于
怎么可靠地用它，以及**怎么不把"我看不见"误判成"它死了"**。

## 基本脚手架

用 `/opt/homebrew/bin/python3`（系统默认那个没有 pyserial）：

```python
import serial, time
p = serial.Serial('/dev/cu.usbserial-10', 115200, timeout=0.3)
time.sleep(0.8); p.reset_input_buffer()

def send(cmd, wait=5):
    p.write((cmd+'\r\n').encode()); p.flush()
    end = time.time() + wait; buf = b''
    while time.time() < end:
        d = p.read(4096)
        if d: buf += d
    return buf.decode('utf-8', 'replace')

print(send('free', 3))
p.close()
```

**固定时长收集，不要按行读。** NSH 的输出没有可靠的结束标记，按行读会在
命令还没跑完时就返回。要提前退出就检测特征串：

```python
while time.time() < end:
    d = p.read(4096)
    if d:
        buf += d
        if b'RADIO WORKS' in buf: break     # 拿到想要的就停
```

## 判"卡死"之前，先确认有没有观察通道

这是本项目代价最大的一课，**误判过一次，撤回过一次**。

`cmocka_driver_block` 压测在 NSH 里是**前台任务**，运行期间 NSH 本来就不回显，
而它的循环里一个字都不打印。"发命令没回显"被当成"系统死了"——实际上后台重跑
889 秒，`ps` 显示任务始终 `Ready`，板子全程健康。那个测试在 120 MB 卡上要跑
233472 次迭代，**推算数小时**，等 300 秒就下结论是不够的。

### 纪律

1. **长任务丢后台**（`命令 &`），给自己留一个能用的 shell。
2. **`ps` 分清状态**：`Ready` = 在跑，`Waiting` = 真卡住。
3. **DTR/RTS 无响应不是死机证据** —— 本板 CEN 没接 CH340 控制线，它永远无效。
4. 判定"死了"之前问一句：**它现在应该有输出吗？**

## 板子健康三件套

```
uptime     # 有没有偷偷复位过（看门狗咬了会归零）
free       # 堆还在不在，有没有泄漏
ps         # 任务状态
```

`uptime` 归零而你没重启，说明**看门狗咬了**——往回查最近改的时基/中断。

> 真实案例：时基写入竞态让比较器永不匹配，现象是"板子答得动却没有时钟"
> （UART 走自己的中断所以照常回显），`sleep` 永不返回，喂狗随之停止，
> 恰好一个看门狗周期后复位。**能回显不等于系统活着。**

## 没有专用命令时，用 `xd` 直读寄存器

NSH 自带 `xd <地址> <字节数>`，返回小端字节序：

```
xd 0x440004cc 16
0000: 09 00 01 00 03 00 00 00 ...   → 0x00010009, 0x00000003, ...
```

⚠️ **读 GPIO 电平前先确认输入使能位**。输入使能关着时，输入锁存器读到的不是
真实焊盘电平——这个假阴性坑过三小时（详见 `hardware-truth` skill）。

## 诊断命令留在树里

本仓的惯例：**调试用的探针不要用完就删**，作为 nsh 命令常驻。

| 命令 | 查什么 |
| --- | --- |
| `bkreg <addr>` | 解码关键寄存器，极性直接写成 on/OFF |
| `timertest` | TIMER 时钟位 + 握手超时 + 计数增量 |
| `flashtest` | flash 控制器 ID、写保护状态 |
| `batttest probe` | 引脚是否被驱动、ADC 通道扫描 |
| `sigtest` | 终端 `c_lflag`、`TIOCSCTTY` 返回值 |

下一个人撞同样的问题时不必重造仪器。**加新驱动就顺手加一个诊断命令**，
挂在 `src/CMakeLists.txt` 的 `nuttx_add_application()` 下。

## 自动化验证的写法

改完驱动后的标准闭环（配合 `flash-firmware` skill）：

```python
# 烧录后
print(send('ls /dev', 3))        # 节点注册了吗
print(send('<你的诊断命令>', 10)) # 功能对吗
print(send('uptime', 2))         # 跑完之后板子还活着吗
```

最后那条容易忘。**测试跑完板子重启了，等于没通过。**
