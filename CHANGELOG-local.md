# F26-QNX-Workshop 本地修改说明

> 针对 `dev/workshop` 分支的本地改动与使用记录（Ideas Clinic / Fall 2026 QNX Workshop）。

## 1. 项目在做什么

在树莓派上运行 **QNX 8.0**，用 **MAX30102** 采集心率与血氧，通过 QNX **pulse IPC** 发给 `alarm-manager` 做告警显示。

- 开发主机：Windows + QNX SDP 8.0（本机路径 `E:\QNXSDP`）
- 交叉编译：`qcc -Vgcc_ntoaarch64le`
- 目标板：`qnxuser@192.168.2.89`（主机名 `qnxpi`）
- 板子上无 git / 完整工具链；源码在主机编译后拷贝二进制部署

官方仓库：`https://github.com/IdeasClinicUWaterloo/F26-QNX-Workshop`  
有效内容分支：`dev/workshop`（`main` 基本只有环境搭建）

---

## 2. 改了哪些文件

| 文件 | 作用 |
|------|------|
| `01-Pulse-Oximeter/src/main.c` | 传感器进程入口、传感器配置、向告警进程上报 |
| `01-Pulse-Oximeter/src/max30102.c` | Maxim 心率 / SpO₂ 算法与有效性判断 |
| `02-Alarm-Manager/src/main.c` | 接收 pulse、刷新显示线程 |
| `02-Alarm-Manager/src/alarm_manager.c` | 告警状态机、滤波、传感器超时重启 |
| `02-Alarm-Manager/src/alarm_manager.h` | 告警模块接口声明 |

---

## 3. 问题与修改对照

### 3.1 `alarm-manager` 狂刷 `waitpid` / Restarting

**现象**

```text
waitpid: No child processes
[ERROR] Restarting pulse oximeter...
```

**原因**

`update_alarm_states()` 在 `ALARM_SENSOR_DISCONNECT` 分支里**每次刷新**都调用 `restart_pulse_oximeter()`，且 PID 未初始化；车间流程里 `pulse-oximeter` 通常由 shell 启动，也不是 alarm 的子进程，`waitpid` 必然失败。

**修改**

- 仅在真正超时（距上次有效采样 > 1.5s）且尚未尝试过时重启一次
- 重启改用 `slay -f pulse-oximeter` + `posix_spawn`
- 断连超时改为按「当前时间 − 上次采样时间」计算（原先只在收到 HR pulse 时更新间隔，进程挂掉后永远测不到超时）

### 3.2 一启动就报 Low HR / Low SpO₂

**原因**

全局变量初始为 `0`，告警阈值为 SpO₂ &lt; 85、HR &lt; 40，立刻误报。

**修改**

- 无效读数使用 `-1` / 不纳入告警
- 告警只对生理合理范围的读数生效
- 需连续确认多次才拉高告警（减少偶发野值误报）
- 显示刷新间隔改为约 200ms（原先 1ms 刷屏）

### 3.3 数值乱跳、卡在 250 BPM / SpO₂ 28

**结论**

不只是显示滤波问题；根因在 **传感器配置 + Maxim 算法缺少有效性门控**。仅在 `alarm-manager` 做中值滤波时，一旦第一个野值进入滤波器，后续正常读数会被当成「突变」全部丢掉，表现为一直卡在 250。

**对照参考（网上常见做法）**

- [SparkFun MAX3010x Example8_SPO2](https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library/blob/master/examples/Example8_SPO2/Example8_SPO2.ino)
- Maxim `spo2_algorithm`（SparkFun 库内 `spo2_algorithm.cpp`）
- 社区常见：`IR` 阈值检测手指、`validHeartRate` / `validSPO2` 标志、BPM 合理范围再显示

**算法 / 配置侧修改（`01-Pulse-Oximeter`）**

| 项 | 原来 | 现在 |
|----|------|------|
| LED 亮度 | 50 | **60**（对齐 Example8） |
| ADC 量程 | 16384 | **4096**（对齐 Example8） |
| 采样 | 100 SPS + avg 4 → 有效 25 Hz | 保持（与算法 `FreqS=25` 一致） |
| 手指检测 | 无 | IR DC 不在合理范围 → 整窗无效 |
| 波峰最小间距 | 4（可算出约 375 BPM） | **8**（上限约 187 BPM） |
| 最少峰数 | ≥2（一个噪声间隔就能出 250） | **≥3**（至少两个间隔） |
| HR 输出范围 | 算出即发 | 仅 **40–180** 标为 valid |
| SpO₂ 输出范围 | 算出即发 | 仅 **70–100** 标为 valid |
| 上报策略 | 无效也发原始数 | `valid=0` 发 **-1**；有效值再做 **4 点平均** |

**告警侧仍保留的轻度处理（第二层）**

- 中值滤波（最近若干点）
- 相对当前显示值的突变丢弃
- 连续拒收过多次后重置滤波器，避免再次锁死

### 3.4 I2C `Permission denied`

**原因**

`/dev/i2c0`、`/dev/i2c1` 属主为 `root:root`，权限常为 `660`；`qnxuser` 不在可访问组。重启后 `chmod` 会丢。

**处理（每次重启后）**

```bash
su -c 'chmod 666 /dev/i2c0 /dev/i2c1'
# 密码: root
```

确认：

```bash
ls -l /dev/i2c1
# 期望: crw-rw-rw-
```

### 3.5 SSH `Corrupted MAC` / 连接被关闭

Windows OpenSSH 与板子 QNX sshd 的默认 MAC 协商不兼容；IPv6 链路本地地址也容易踩坑。

**推荐连接方式**

```bash
ssh -4 -m hmac-sha2-256 qnxuser@192.168.2.89
# 密码: qnxuser
```

可选 `~/.ssh/config`：

```text
Host qnxpi
  HostName 192.168.2.89
  User qnxuser
  AddressFamily inet
  MACs hmac-sha2-256
```

---

## 4. 开发与部署流程

### 4.1 主机环境

```bat
E:\QNXSDP\qnxsdp-env.bat
cd C:\Users\BillL\Projects\F26-QNX-Workshop
make build
```

产物：

- `01-Pulse-Oximeter/build/aarch64le-debug/pulse-oximeter`
- `02-Alarm-Manager/build/aarch64le-debug/alarm-manager`

### 4.2 部署到板子

将上述两个二进制拷到 `/home/qnxuser/`，并 `chmod +x`。

### 4.3 板上运行

```bash
su -c 'chmod 666 /dev/i2c0 /dev/i2c1'   # 密码 root

slay -f pulse-oximeter
slay -f alarm-manager

./alarm-manager &
sleep 1
./pulse-oximeter
```

或写日志后观看：

```bash
./alarm-manager >/tmp/alarm-manager.log 2>&1 &
./pulse-oximeter >/tmp/pulse-oximeter.log 2>&1 &
tail -f /tmp/alarm-manager.log
```

停止：

```bash
slay -f pulse-oximeter
slay -f alarm-manager
```

---

## 5. 预期测试结果

| 状态 | 预期输出 |
|------|----------|
| 未放手指 | `HR=--` / 告警侧 `-1`，`[Alarm] System OK` |
| 手指轻轻贴稳 5–10 秒 | 大约 60–100 BPM，SpO₂ 大约 95–100 |
| 异常（应已缓解） | 不应长期卡在 250 BPM / SpO₂ 28；不应狂刷 `waitpid` |

注意事项：

- 手指不要用力压、不要晃
- 刚贴上前几秒可能仍是无效读数（在攒有效窗口）
- MAX30102 的 Maxim/SparkFun 算法是教学级实现，不是医疗设备

---

## 6. 用到的工具与参考

- QNX SDP 8.0、`qcc`、`make`
- QNX IPC：`name_attach` / `name_open` / `MsgSendPulse` / `MsgReceive`
- SparkFun MAX3010x Example8_SPO2 传感器设置
- Maxim Integrated `spo2_algorithm`（心率波谷检测 + SpO₂ 查表）
- 社区常见手指检测（IR 阈值）与 `valid` 标志用法

---

## 7. 一句话总结

先修了 `alarm-manager` 的致命逻辑 bug（乱重启、误报、超时算错），再按 **SparkFun / Maxim 参考** 修正 `pulse-oximeter` 的传感器配置与算法有效性门控；告警侧滤波只是辅助，真正消除 250 BPM 一类野值靠的是算法侧的手指检测、峰间距、峰数量、`valid` 标志和生理范围检查。
