# RM27 自定义控制器 · 部署与联调速查

> 适用范围：NUC 上克隆 `feature/rm27-custom-control` 分支，部署 `rm_EGA` 客户端，接入旧
> 自定义控制器（STM32 → USB 串口 → `rm_EGA` → MQTT `CustomControl`）并做分阶段联调。
> 本文只描述联调操作，**不代表实机控制已经验收通过**。首次上电请先禁能机械臂执行机构，
> 只验证数据接收。
>
> 验证状态（截至本文整理时）：
>
> | 阶段 | 内容 | 状态 |
> | --- | --- | --- |
> | 软件模拟 | 串口解析、30B 透明转发、QoS 0 发布、共享限频、失败日志、本机 Broker 链路 | **已通过**（Task 8-J 软件验收 PASS；`TestCustomControllerMqtt` 连续 15/15） |
> | 官方视频图传 | 主 HEVC / 辅助 H.264 图传接收与解码（既有图传工作线） | **已完成**（本分支未改动图传代码，沿用既有验证结论） |
> | 官方 MQTT 接入 | 真实官方 Broker、机器人身份与权限 | **已连接**；现场抓到客户端发送的相关 TCP 数据，Broker 处理与身份权限仍需核验 |
> | 控制下行 `0x0311` | 图传发送端 → C 板接收、超时停控、机械臂动作 | **待电控/实机联调** |

---

## 1. 启动

### 1.1 在 NUC 上克隆

```bash
# HTTPS（需要仓库访问权限）
git clone --branch feature/rm27-custom-control \
  https://github.com/darkttsave/rm_EGA.git ~/rm_EGA
cd ~/rm_EGA

# 或 SSH（已配置 SSH key 时）
# git clone --branch feature/rm27-custom-control \
#   git@github.com:darkttsave/rm_EGA.git ~/rm_EGA

git branch --show-current        # feature/rm27-custom-control
git log -1 --oneline             # 应包含 custom-control 功能提交
```

> 只使用 `origin`（`darkttsave/rm_EGA`）；`upstream` 是复旦原始仓库，不要向其推送。

### 1.2 依赖（Ubuntu 24.04）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config git \
  protobuf-compiler libprotobuf-dev libabsl-dev libpaho-mqtt-dev \
  qt6-base-dev qt6-declarative-dev qt6-multimedia-dev \
  qt6-serialport-dev qt6-svg-dev \
  qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
  qml6-module-qtquick qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-templates \
  qml6-module-qtquick-window \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

- 完整依赖列表（含字体、xcb 运行时包）见 `docker/client.Dockerfile`；带桌面的 NUC 通常已具备。
- 客户端需要图形会话（Qt GUI）；没有显示环境时无法正常启动。

### 1.3 本地配置

```bash
cd ~/rm_EGA
cp config.example.json config.json     # 首次部署；已有现场配置请勿覆盖
python3 tools/release/check_example_config.py config.json
```

`config.json` 由 Git 忽略，只属于本机。至少核对 `network` 下的：

- `mqtt_broker` / `mqtt_port`：官方接口地址与端口（见第 2 节，可用环境变量覆盖）；
- `client_robot_id`：与官方操作端绑定身份一致（可用 `RM_CLIENT_ROBOT_ID` 覆盖）。

### 1.4 构建客户端

```bash
cd ~/rm_EGA
cmake --preset release
cmake --build --preset release
ls -l build/release/RoboMasterClient2025
```

- 使用 preset 时，可执行文件在 **`build/release/RoboMasterClient2025`**。
- 如果显式指定构建目录（`cmake -S . -B build ...`），可执行文件在 **`build/RoboMasterClient2025`**。
- 构建成功可在对应目录直接运行；首次运行前再次确认实际加载的 `config.json`。

### 1.5 识别控制器串口

控制器通过 USB 转串口连接（115200、8N1、无流控）。插拔前后对比：

```bash
ls -l /dev/serial/by-id/           # 首选：稳定设备路径（含厂商/序列号）
ls -l /dev/ttyUSB* /dev/ttyACM*    # 备选：确认新出现的设备节点
dmesg | tail -20                   # 查看内核识别日志
udevadm info -q property -n /dev/ttyUSB0 | grep -E 'ID_VENDOR|ID_MODEL|ID_SERIAL'
```

- 优先使用 `/dev/serial/by-id/<设备>`，避免重启后 `ttyUSB0/ttyUSB1` 编号变化。
- 当前用户需要有串口权限：`groups | grep dialout`；没有则
  `sudo usermod -aG dialout "$USER"` 后重新登录。
- **不要**把串口路径写死为 `/dev/ttyUSB0`，必须替换成确认属于控制器的实际路径。

### 1.6 启动 rm_EGA（启用控制器转发）

```bash
cd ~/rm_EGA

env -u http_proxy -u https_proxy -u all_proxy \
    -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
  RM_MQTT_BROKER=192.168.12.1 \
  RM_MQTT_PORT=3333 \
  RM_CLIENT_ROBOT_ID=<按实际机器人填写> \
  RM_CUSTOM_CONTROLLER_PORT=<实际控制器串口> \
  RM_CUSTOM_CONTROLLER_FORWARD=1 \
  ./build/release/RoboMasterClient2025
```

说明：

- `env -u ...` 只对当前进程清除代理变量，避免 Paho 把 MQTT 连接错误地导向本机 HTTP 代理；
  **不要**修改系统全局代理配置。
- 客户端默认**不**创建转发链路；必须同时设置 `RM_CUSTOM_CONTROLLER_FORWARD=1` 与
  `RM_CUSTOM_CONTROLLER_PORT`。
- 启动成功时日志应出现 `Custom controller forwarding started: <设备路径>`。

---

## 2. 参数

| 环境变量 | 示例 | 说明 |
| --- | --- | --- |
| `RM_CUSTOM_CONTROLLER_PORT` | `/dev/serial/by-id/...` | 旧控制器 USB 串口路径；115200、8N1；必须按实际设备替换 |
| `RM_CUSTOM_CONTROLLER_FORWARD` | `1` | 是否启用转发；**默认禁用**，只有显式 `1` 才启用 |
| `RM_MQTT_BROKER` | `192.168.12.1` | 官方接口地址；普通 Mosquitto 只能验证本机 MQTT，不会转发到 C 板 |
| `RM_MQTT_PORT` | `3333` | 自定义终端 MQTT 端口（官方接口约定；本机模拟常用 3333） |
| `RM_CLIENT_ROBOT_ID` | 按实际填写 | 机器人身份，**必须**与官方操作端绑定/注册身份一致；红蓝方 ID 不同，示例值不能当默认 |

核对要点：

- 以上五个值都与现场强相关：机器人 ID 和串口路径必须按本次测试对象确认，**不能把示例值当默认**。
- 官方接口地址、自定义终端 IP 与路由由队内网络管理员核实；不要在未确认时强行修改 NUC 网络。
- 协议侧约定（当前实现）：`CustomControl` 单条载荷上限 60B（本链路实际两类：控制器 30B、
  前哨站 10B），QoS 0，所有 `CustomControl` 发送共享 14ms 限频（≤75Hz）；控制器串口 115200 8N1。
- 图传发送端 → 机器人串口的裁判帧为 921600 8N1、机器人端 `0x0311`（与控制器 USB 串口不同，
  以电控/图传负责人确认为准）。

---

## 3. 验证

按阶段推进，**不要跳级**：A 通过不代表 C 通过；本机 Broker 收到 Topic 不代表真实 C 板收到
`0x0311`。

| 阶段 | 验证内容 | 通过标准 | 当前状态 |
| --- | --- | --- | --- |
| A | 软件模拟：串口→MQTT 透明转发 | 30B 逐字节一致、QoS 0 发布、限频与失败日志正常 | **已通过**（Task 8-J） |
| B | 客户端连接官方 MQTT Broker | 连接成功，`CustomControl` Topic 有 PUBLISH | 已连接并抓到相关 TCP 数据；待核验 Broker 处理 |
| C | 图传发送端 → C 板控制下行 | C 板收到 `cmd_id=0x0311`、30B、CRC 正确 | 待电控/实机 |
| D | 执行机构与安全行为 | 断链超时停控、恢复只用新帧、机械臂动作符合预期 | 待实机 |

### 3.1 客户端能否连接官方 MQTT Broker

```bash
# 仅验证 TCP 可达（不代表 MQTT 鉴权/主题正确）
nc -vz -w 2 192.168.12.1 3333

# 启动客户端（见 1.6）后检查运行日志
cd ~/rm_EGA
ls -dt tmp/runtime_* | head -1          # 最新一次运行日志目录
grep -E "MQTT|Mqtt" "$(ls -dt tmp/runtime_* | head -1)/runtime.log" | tail -20
```

- 以日志中出现 MQTT 连接/订阅成功、且 Broker 侧能看到 `CustomControl` 为准；仅 `publish()`
  返回成功不能证明 Broker/机器人收到。
- MQTT 报文中转日志目录：`tmp/log/mqtt_full_*.jsonl`（相对启动目录）。注意 CustomControl 的
  逐帧发送**刻意不写入**该 JSONL，避免高频控制日志；30B 发布请用 3.3 的 Wireshark 方式确认。

### 3.2 是否成功打开实体控制器串口

- 成功：日志出现 `Custom controller forwarding started: <设备路径>`。
- 失败：
  - `Custom controller device path is required`：未设置/空 `RM_CUSTOM_CONTROLLER_PORT`；
  - `Failed to open custom controller <路径>: ...`：路径错误、权限不足或被占用；
  - `Custom controller serial error: ...`：运行中读错误，会关闭串口并停止转发。
- 串口被占用排查：

```bash
fuser -v <实际串口路径>
lsof <实际串口路径>
```

  若被官方客户端、Validator 或串口调试工具占用，先退出对应程序，再启动 `rm_EGA`；不要同时运行
  两个占用同一串口的程序，也不要随意 kill 未知系统进程。

### 3.3 是否接收旧 `0x0302` 39B 帧，并向 `CustomControl` 发布 30B

旧控制器发送标准 39B 帧：`A5 | data_length(30, LE) | seq | CRC8 | cmd_id=0x0302 | 30B Payload | CRC16`。
客户端解析成功后只把 **30B Payload** 作为 Protobuf `CustomControl.data` 发出（不带旧帧头/CRC）。

**方式一：Wireshark（Windows）确认 MQTT PUBLISH**

以下为本次现场实例：Windows 官方选手端 `192.168.12.1`、Ubuntu 自定义终端
`192.168.12.2`、MQTT TCP 端口 `3333`，Topic 为 `CustomControl`。IP 和端口以实际网络为准。

1. 在 Windows Wireshark 中选择与 Ubuntu 通信的以太网网卡并开始抓包，显示过滤器输入
   `tcp.port == 3333`。建议**先抓包、再启动客户端**，以观察 CONNECT / CONNACK。
2. Wireshark 能识别 MQTT 时，使用 `mqtt.msgtype == 2` 找到 CONNACK，检查返回码为 `0`。
   不能识别时可查看原始 TCP Payload；普通 TCP ACK 不是 MQTT CONNACK，也不是应用接收确认。
3. 查找本次实测可用的显示过滤器：

   ```text
   ip.src == 192.168.12.2 && tcp.dstport == 3333 && tcp contains "CustomControl"
   ```

   操作控制器时应持续出现匹配数据包；核对源 IP 为 Ubuntu、目的 IP 为 Windows。
   `CustomControl` 是 MQTT Topic。能识别 MQTT 时也可使用 `mqtt.topic == "CustomControl"`
   或 `mqtt.msgtype == 3` 查找 PUBLISH，展开报文检查 QoS、Topic 和 Payload。
4. 控制器原始数据为 30B，对应 Protobuf Payload 通常是 `0A 1E` + 30B，共 32B；10B 前哨站
   Payload 为 `0A 0A` + 10B。单条 30B 控制器数据的 QoS 0 MQTT PUBLISH 通常为 49B：
   MQTT 固定头 2B + Topic 长度字段 2B + `CustomControl` 名称 13B + Protobuf 头 2B + 原始
   数据 30B。将原始 30B 与控制器发送端逐字节比较。
5. **不要只凭 TCP Length 判断**：TCP 可能合并或拆分 MQTT 报文，过滤器也可能漏掉 Topic 被
   拆分的片段。应结合 Topic、完整 MQTT 报文结构和 Payload 验证。无法识别 MQTT 时，可用
   `tcp contains "CustomControl"` 查找，在 TCP 数据区查看十六进制，并用 Follow TCP Stream
   查看完整通信；必要时保存 pcapng 供授权工具离线解析。

若 Windows 无法直接抓到链路，也可在 Ubuntu 网卡用
`sudo tcpdump -i <官方接口网卡> -s 0 -w customcontrol.pcap 'tcp port 3333'` 抓包，或使用
Wireshark 的 SSH 远程抓包；将抓包文件按现场授权方式在 Windows 打开。

判定时区分：① 本机 MQTT publish 接口接受；② Windows 网络接口抓到 MQTT PUBLISH；
③ 官方 Broker 处理消息；④ C 板收到 `0x0311`。本次现场已有前两项的相关证据，抓包
本身不能证明 Broker 已处理，更不能证明 C 板已接收；C 板接收仍待验证。

抓包仅用于授权联调排障；原始抓包和现场 IP/主机名不得提交仓库或擅自外传。

**方式二：本地订阅端（仅本机模拟环境）**

```bash
mosquitto_sub -h 127.0.0.1 -p 3333 -t CustomControl -q 1 -F '%t|%l|%q|%X'
```

本地 Mosquitto 验证只说明本机链路正常，**不能**代表官方 Broker 或 C 板收到。

### 3.4 由电控验证 C 板是否收到 `0x0311`

图传发送端 → 机器人串口为 921600 8N1，裁判帧结构与旧协议一致：

```text
A5 | data_length(2, LE) | seq(1) | CRC8(1) | cmd_id(2, LE)=0x0311 | data(30) | CRC16(2, LE)
```

请电控/图传负责人用图传发送端串口抓包工具或机器人端日志确认：

- 抓到 `cmd_id = 0x0311`（小端 `11 03`）；
- `data` 长度 30B，内容与 Wireshark 中 30B Payload 一致；
- CRC8（帧头前 4B）与 CRC16（整帧去尾 2B）校验正确；
- 机器人端按内容长度区分 **30B 控制器** 与 **10B 前哨站**（两者都可能出现在 `CustomControl`）。

再次强调：A/B 阶段通过 ≠ C 阶段通过；必须由 C 板侧给出接收与 CRC 校验证据。

### 3.5 软件模拟链路（无控制器时可先做）

自动测试（本机 Broker + PTY 伪串口，覆盖 30B 透明转发、10B 前哨站、限频与失败日志）：

```bash
sudo apt-get install -y mosquitto
cd ~/rm_EGA
QT_QPA_PLATFORM=offscreen \
  env -u http_proxy -u https_proxy -u all_proxy \
      -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
  ctest --test-dir build/release \
    -R '^(TestCustomControllerMqtt|TestCustomControllerParser)$' \
    --output-on-failure
```

以上即 Task 8-J 已通过的软件层验证（`TestCustomControllerMqtt` 连续 15 次通过，`TestCustomControllerParser`
通过）。模拟器链路（比赛状态/命令/图传模拟）见 `sim/README.md`：

```bash
./sim/run_sim.sh
```

---

## 4. 恢复

### 4.1 关闭自定义控制器转发

- 退出客户端；或下一次启动时不要设置 `RM_CUSTOM_CONTROLLER_FORWARD`，或显式设为 `0`：

```bash
cd ~/rm_EGA
RM_CUSTOM_CONTROLLER_FORWARD=0 ./build/release/RoboMasterClient2025
```

- 默认配置下客户端不会打开串口、不会创建转发链路。

### 4.2 释放串口

```bash
# 退出客户端后确认没有进程占用
fuser -v <实际串口路径>
lsof <实际串口路径>
```

若仍被占用，按顺序处理：

1. 退出再次打开的客户端/串口调试工具；
2. 退出可能占用串口的官方客户端或 Validator 工具；
3. 重新插拔控制器 USB（等待 `dmesg` 重新识别）；
4. 换一个 USB 口后重新确认 `/dev/serial/by-id/` 路径；
5. 不要 kill 未知系统进程、不要卸载内核驱动。

### 4.3 恢复本地模拟器配置

```bash
# 1) 取消所有控制器转发变量（只对当前 shell 会话生效变量已随进程退出）
unset RM_CUSTOM_CONTROLLER_PORT RM_CUSTOM_CONTROLLER_FORWARD

# 2) 如现场配置需要完全回到公开示例：先备份，再覆盖
cp config.json config.json.bak.$(date +%Y%m%d-%H%M%S)
cp config.example.json config.json

# 3) 启动本地模拟器与客户端（本机 MQTT）
./sim/run_sim.sh
./build/release/RoboMasterClient2025
```

- `config.json` 是本地文件，不要提交；恢复模拟器后客户端默认连接本机 Broker
  （示例配置为 `127.0.0.1:3333`，以实际配置为准）。

### 4.4 检查本地修改

```bash
cd ~/rm_EGA
git status --short
git diff -- docker/mosquitto-local.conf
```

- `docker/mosquitto-local.conf` 是**本地保留的未提交修改**，不要提交、暂存、恢复或 stash。
- 联调产生的 `config.json`、`tmp/`、`build/`、日志与抓包文件都不进入 Git。
- 恢复现场**不要**使用 `git reset --hard`、`git clean`、`git stash`；需要丢弃某个文件的本地
  改动时，先人工备份，再按需处理后用 `git status` 核对。

### 4.5 异常排查顺序

1. 先禁能机械臂执行机构（安全第一）；
2. 串口：`ls -l /dev/serial/by-id/`、`fuser`、`dmesg`；
3. 身份：`RM_CLIENT_ROBOT_ID` 与官方绑定是否一致；
4. 网络：官方接口地址/端口、`nc -vz`、Wireshark 是否看到 `CustomControl`；
5. 控制下行：图传发送端串口 921600、`0x0311`、CRC、C 板接收日志；
6. 记录：保留 Wireshark pcap、客户端日志与 C 板日志的时间窗，便于对端核对（脱敏后再共享）。
