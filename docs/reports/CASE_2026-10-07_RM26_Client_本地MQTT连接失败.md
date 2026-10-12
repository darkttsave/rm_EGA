# CASE_2026-10-07_RM26_Client_本地 MQTT 连接失败

> 案例记录现场事实；只有经过验证的可迁移结论才进入通用问题手册。

## 1. 现场目标

- 车辆/身份：本机模拟器，`robot_id=1`，客户端 `clientId=1`
- 系统：Ubuntu 24.04
- 代码版本：`main`，`e291f2f`
- 配置文件：`config.json`
  - Broker：`tcp://127.0.0.1:3333`
  - MQTT 版本：3.1.1
  - `cleanSession=1`
  - `keepAliveInterval=20`
  - `connectTimeout=2`
- MQTT 库：Ubuntu 包 `libpaho-mqtt-dev/libpaho-mqtt1.3 1.3.13-1build2`
- 实际链接：`/lib/x86_64-linux-gnu/libpaho-mqtt3c.so.1`
- 目标：解决客户端首次连接本地 MQTT Broker 返回 `rc=-1`、画面无法通过 MQTT 更新的问题。

## 2. 原始现象

- Mosquitto 正常监听 `127.0.0.1:3333`，`nc -vz 127.0.0.1 3333` 成功。
- 模拟器能连接 Broker，并持续发布 `GameStatus`、`RobotStaticStatus`、`GlobalUnitStatus`、`RuneStatusSync`、`AirSupportStatusSync`、`RadarInfoToClient` 等消息。
- 客户端创建 Paho client 成功，但 `MQTTClient_connect()` 返回 `-1`。
- 失败时客户端回退到 UDP；画面是否变化不能单独作为 MQTT 成功的判断依据。
- 失败时 Broker 没有出现客户端完成 MQTT 握手的 `New client connected ... as 1`。
- 早期 Paho trace 观察到 `MQTTClient_connectURIVersion ... (-1)` 后清理 socket/session；部分失败未进入有效 CONNECT/CONNACK 阶段。

## 3. 复现条件

| 条件 | 内容 |
|---|---|
| Broker | Mosquitto，`127.0.0.1:3333` |
| 数据源 | 本机 RM26 模拟器，`robot_id=1` |
| 客户端 | `build/release/RoboMasterClient2025` |
| 发生频率 | 带终端代理环境启动时可稳定出现；清除代理环境后未再复发 |
| 相关环境 | `http_proxy/https_proxy/HTTP_PROXY/HTTPS_PROXY` 指向 `127.0.0.1:7897`；虽配置 `no_proxy=localhost,127.0.0.1`，当前 Paho 连接仍受代理变量影响 |

## 4. 基线证据

- Broker/模拟器日志：`mqtt_broker.log`
- 客户端运行日志：`tmp/runtime_*/runtime.log`
- Paho trace：`/tmp/paho_original_no_proxy.log` 及此前 `/tmp/paho_trace.log`
- 客户端错误：`MqttManager: Failed to connect, rc=-1`
- Broker 基线：模拟器持续 PUBLISH/PUBACK，说明 Broker、端口和模拟器 MQTT 路径正常。
- 原始代码清除代理后的成功证据：
  - `tmp/runtime_20261007-182401/runtime.log`
    - `18:24:05.228 MqttManager: Connected successfully`
    - `18:24:05.495 MainWindow: MQTT bootstrap success, clientId="1"`
    - `18:24:05.495 MainWindow: MQTT mode enabled, UDP listener disabled`
  - `tmp/runtime_20261007-194559/runtime.log`、`runtime_20261007-195134/runtime.log`、`runtime_20261007-195340/runtime.log` 也记录了原始代码成功启动 MQTT。
  - Broker 记录：`New client connected ... as 1 (p2, c1, k20)`。

## 5. 分层排查

| 数据流节点 | 预期 | 实际 | 是否通过 | 证据 |
|---|---|---|---|---|
| Broker 监听 | `127.0.0.1:3333` 可连接 | Mosquitto 正常监听，`nc` 成功 | 通过 | 端口检查、Broker 日志 |
| 模拟器发布 | 持续向 Broker 发布比赛状态 | 多个 Topic 持续 PUBLISH/PUBACK | 通过 | `mqtt_broker.log` |
| Paho 动态库 | 客户端链接系统 Paho C 库 | 链接 `libpaho-mqtt3c.so.1` 1.3.13 | 通过 | `ldd`、包版本 |
| Paho create | 创建 client handle | create 成功 | 通过 | 客户端/Paho trace |
| 客户端 MQTT connect（带代理） | 完成 TCP、CONNECT、CONNACK | 返回 `rc=-1`，Broker 无 `clientId=1` | 不通过 | 客户端日志、Broker 日志 |
| 客户端 MQTT connect（清除代理） | 完成 MQTT 握手 | `Connected successfully` | 通过 | runtime 日志 |
| Broker 客户端认证 | 出现 `clientId=1` | `New client connected ... as 1` | 通过 | Broker 日志 |
| 应用传输模式 | MQTT 成功后禁用 UDP 数据监听 | `MQTT mode enabled, UDP listener disabled` | 通过 | runtime 日志 |
| UI 数据 | Topic 驱动画面更新 | 自定义客户端正常显示 | 通过 | 现场观察 |

## 6. 假设与实验记录

| 轮次 | 假设 | 唯一主要改变量 | 预期 | 结果 | 结论 |
|---|---|---|---|---|---|
| 1 | Broker 未启动或端口不通 | 检查监听、`nc`、Broker 日志 | 若成立则 TCP 失败 | 监听正常，模拟器持续发布 | 排除 Broker/端口 |
| 2 | Paho 未正确安装或链接错误 | 检查包版本和 `ldd` | 若成立则 create/link 异常 | Paho 1.3.13 正确安装并链接 | 排除安装/链接 |
| 3 | clientId、MQTT 3.1.1、URI 或超时参数不兼容 | 保持配置，用独立/受控路径连接 | 若成立则 Broker 拒绝或各路径均失败 | 相同协议参数能够连接 | 不是直接原因 |
| 4 | callback/QThreadPool 启动与生命周期问题导致首次失败 | 试验专用 worker、同步 receive、attempt ID | 修改后应稳定连接 | 实验版本能够连接并通过测试 | 证明替代路径可用，但一次改动多个变量，不能单独证明原架构是本次直接原因 |
| 5 | 桌面代理变量被 Paho 用于本地 MQTT | 原始代码下显式注入代理 | 应复现 `rc=-1` | 原始代码失败，Broker 未完成 `clientId=1` 连接 | 支持代理假设 |
| 6 | 仅清除代理即可恢复 | 保持仓库原始代码，只清除代理变量 | 原始代码应完成 MQTT 握手 | 多次 `Connected successfully`，Broker 确认 `clientId=1`，UI 正常 | 确认为本次直接原因 |

### 失败的中间尝试

- 曾把 `MQTTClient_connectOptions.httpProxy/httpsProxy` 设置为空字符串，结果 `MQTTClient_connect()` 在约 0 ms 内返回 `-1`。Paho 1.3.13 将非空指针指向的空字符串视为无效显式代理配置；该方式不可用。
- 曾实现进程内临时清除代理变量的实验方案。它能通过连接测试，但环境变量属于进程全局状态，可能影响其他网络线程，因此没有作为正式修复保留。

## 7. 最终处理

- 直接根因：启动客户端的终端环境包含 HTTP/HTTPS 代理变量；在当前 Ubuntu 24.04 + Paho 1.3.13 组合中，本地 `tcp://127.0.0.1:3333` MQTT 连接仍受到这些变量影响，导致首次连接 `rc=-1`。
- 最终代码修改：无。仓库源码已恢复到原始版本。
- 当前最小处理方式：启动客户端进程时清除代理变量：

  ```bash
  env -u http_proxy -u https_proxy -u all_proxy \
      -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
      ./build/release/RoboMasterClient2025
  ```

- 建议的长期处理：从客户端专用启动脚本或桌面启动项中清除代理变量；不要为本地 MQTT 修改全进程代理环境，也不要在未完成控制变量验证前改写 MQTT 收包架构。
- 回退方式：无需回退正式源码；实验性 MQTT worker 修改保存在 `stash@{0}: codex-mqtt-experiment-20261007`，未应用到工作区。

## 8. 回归结果

- 已验证场景：
  - 原始代码 + 本机 Mosquitto + 本机模拟器 + 清除代理变量。
  - MQTT 成功连接，Broker 识别 `clientId=1`。
  - `MQTT mode enabled, UDP listener disabled`，排除仅由 UDP fallback 显示画面的情况。
  - 自定义客户端能够正常显示模拟器数据。
  - 问题在后续现场使用中未再复发。
- 未验证/未形成正式结论：
  - 原始 `QThreadPool + 裸 this + 共享 m_client` 在所有退出、登录和快速切换时序下的安全性。
  - callback 模式、QThreadPool 和专用 QThread 的严格单变量性能/稳定性对比。
  - 代理开启且不清除环境变量时的长期兼容方案。
- 独立潜在风险：
  - 排查期间曾观察到 connect 尚未返回时发生 stop/login/destroy 的交错风险。它不是本次稳定首次失败的直接原因，但原实现缺少显式的 in-flight connect 生命周期保护，建议另立案例，用“TCP 已接受但不返回 CONNACK”的假 Broker 做确定性测试。
  - `tmp/runtime_20261007-195340/runtime.log` 曾记录一次成功连接后又进入连续 `rc=-1` 重连；当时外部状态未被单独固定，不能据此归因，保留为后续观察项。

## 9. 手册沉淀

- [x] 记录可迁移结论：Linux 下排查本地 Paho MQTT 连接时，必须检查进程实际继承的代理环境，不能仅依赖 `no_proxy` 或代理软件界面状态。
- [x] 记录判断 MQTT 成功的证据链：客户端 `MQTT bootstrap success` + `MQTT mode enabled, UDP listener disabled` + Broker `New client connected ... as <clientId>`。
- [x] 记录失败方案：不要用 `httpProxy=""` 表示禁用代理。
- [ ] 将无代理启动方式固化到客户端专用启动脚本/桌面启动项。
- [ ] 为 MQTT connect/destroy 生命周期竞态另立独立案例。

