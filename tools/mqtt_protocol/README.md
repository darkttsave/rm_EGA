# MQTT 监听与协议解析工具

一组**只订阅、不发布**的离线观察工具：连上 MQTT Broker，把 `CustomByteBlock` 和
`RobotPathPlanInfo` 这两类真实需要的消息解析成人能看的字段，其余 Topic 只保存原文。

它用于电控/客户端联调时回答一个问题：**"链路上实际发出的字节到底是什么？"**
它不替代客户端解析，也不代表官方链路已经验证通过——固件侧真实链路仍是
哨兵 NUC → C 板 → 官方控制器/裁判系统 → 选手端 → RM26_Client。

---

## 1. 工具解决什么问题

- 现场只有一个 Broker，想看某条消息实际内容时，不必改客户端、不必抓包；
- 协议字段用仓库既有的 `sim/robomaster_pb2.py` 解析，不手工拼 wire format；
- 未知 Topic 不猜协议，原样保存 `payload`，事后仍能复现；
- 所有工具**只订阅**，不会向 Broker 写入任何东西，因此不会干扰比赛链路。

## 2. 分层与职责边界

```text
tools/mqtt_protocol/
├── receiver.py              # 只负责 MQTT 传输
├── decoders.py              # 按 Topic 白名单解析 Protobuf 业务消息
├── inspector.py             # 通用观察入口：receiver + decoders
├── custom_byte_validator.py # CustomByteBlock 的 4 字节协议校验器
├── protocol_specs/
│   └── sentry_bt_status_v1.json
└── tests/
```

| 模块 | 只做什么 | 明确不做 |
| --- | --- | --- |
| `receiver.py` | 连接 Broker、用独立 client-id 订阅一个或多个 Topic、接收、超时/停止、统计条数，把消息包成 `MessageEnvelope(topic, payload, received_at, qos, retain)` | 不解析任何业务协议、不判断 intent、不还原路径、不引用 GameData/QML、**不发布** |
| `decoders.py` | 用白名单把某 Topic 的字节解析成字段 + 检查结果 | 不碰 MQTT、不自动解析 `robomaster.proto` 的全部消息 |
| `inspector.py` | 把两者拼起来：白名单 Topic 调 decoder，其余 Topic 只输出原始信息 | 不猜未知协议、不实现回放、不做 UI |
| `custom_byte_validator.py` | 按 JSON 协议描述校验 `CustomByteBlock.data` 的 4 字节布局 | 不扩大 JSON 协议范围、不解析其它 Topic |

关键点：**接收层不认识任何协议**。`receiver` 可以被任意新工具复用；新增协议只动
`decoders.py` 的白名单，不需要改传输层。

## 3. 环境准备

### 3.1 依赖与版本要求

| 依赖 | 版本要求 | 为什么是这个范围 |
| --- | --- | --- |
| Python | ≥ 3.11 | 与 `sim/pyproject.toml` 的 `requires-python` 一致 |
| `paho-mqtt` | ≥ 2.0 | 代码使用 `CallbackAPIVersion.VERSION2`，该 API 由 paho 2.0 引入 |
| `protobuf` | ≥ 6.33.2, < 7 | `sim/robomaster_pb2.py` 的 gencode 为 6.33.2，导入时会自校验运行时版本 |

`protobuf` 的范围与 `sim/requirements.txt`、`sim/pyproject.toml` 保持一致（同一口径，
由 `tools/release/check_sim_protobuf_runtime.py` 从 pb2 生成代码推导）。

**不需要** Qt、系统 `protoc` 或 ROS：这些工具只用到 Python 标准库加上述两个包。

### 3.2 用哪个解释器

两条等价路径，按手头情况选一条：

- **A. 复用模拟器 venv**：若已按 [`sim/README.md`](../../sim/README.md) 建好 `sim/.venv`，
  它已包含 `paho-mqtt 2.1.0` 与 `protobuf 6.33.6`，满足要求，直接用
  `sim/.venv/bin/python` 即可，不需要再安装任何东西。
- **B. 专用 venv `.venv-mqtt`**：只想跑这些工具时更轻量，步骤见 3.3 / 3.4。

`.venv-mqtt/` 与 `sim/.venv/` 都被 `.gitignore` 忽略，**不会随 clone 出现**，
换机器后必须重新创建。

### 3.3 创建虚拟环境（路径 B）

在仓库根目录执行：

```bash
python3 -m venv .venv-mqtt
source .venv-mqtt/bin/activate
python -m pip install --upgrade pip setuptools wheel
```

Windows PowerShell：

```powershell
py -3.11 -m venv .venv-mqtt
.venv-mqtt/Scripts/Activate.ps1
python -m pip install --upgrade pip setuptools wheel
```

与仓库其余部分保持一致：**不要**用 `sudo pip` 或 `--break-system-packages` 安装依赖。

### 3.4 安装依赖（路径 B）

```bash
python -m pip install 'paho-mqtt>=2.0' 'protobuf>=6.33.2,<7'
```

`<7` 的上限不是随手写的：`sim/robomaster_pb2.py` 导入时会调用
`ValidateProtobufRuntimeVersion`，主版本不符会直接失败。安装后即得到验证过的组合
（`paho-mqtt 2.1.0` + `protobuf 6.33.2`）。

### 3.5 自检

```bash
python -c "import paho.mqtt, google.protobuf as g; print('paho', paho.mqtt.__version__); print('protobuf', g.__version__); print(g.__file__)"
python -m tools.mqtt_protocol.inspector --help
python -m tools.mqtt_protocol.custom_byte_validator --help
```

期望输出类似（路径随你的 venv 位置而变）：

```text
paho 2.1.0
protobuf 6.33.2
/<仓库路径>/.venv-mqtt/lib/python3.12/site-packages/google/protobuf/__init__.py
```

**必须用 `-m` 运行**：这些工具是同包内的模块，依赖包内相对导入；
直接 `python tools/mqtt_protocol/inspector.py` 不保证可用。

`--help` 不导入任何第三方依赖（依赖是延迟导入），裸解释器也能执行，
可用来判断脚本本身是否完好。

若 `g.__file__` 不落在你所选的 venv 内，说明 `PYTHONPATH` 上有别的 `protobuf` 抢先
（`PYTHONPATH` 在 `sys.path` 中先于 site-packages）。本机 ROS 的 `PYTHONPATH` 不含
`google` / `paho`，因此不冲突；换机器时用这条命令确认最稳妥。

### 3.6 依赖缺失或版本不符

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `No module named 'google'` 且提示 `protobuf>=6.33.2,<7` | 用了没装依赖的系统 `python3` | 改用 3.2 选定的 venv 解释器 |
| `未安装 paho-mqtt，无法连接 broker` | 缺 `paho-mqtt`；**只在真正连接时**才暴露，解析与 `--help` 不受影响 | 按 3.4 安装 |
| protobuf 版本不符（过旧或 7.x） | 未按范围安装 | 按 `>=6.33.2,<7` 重装 |
| `.venv-mqtt/bin/python: No such file or directory` | venv 未创建（被 gitignore，不随 clone） | 按 3.3 创建 |
| `attempted relative import with no known parent package` | 直接运行了脚本文件 | 改用 `python -m tools.mqtt_protocol.<模块>` |

## 4. 当前支持的 Topic 与协议

**只有下面两个**，不多不少：

| Topic | 协议 | 说明 |
| --- | --- | --- |
| `CustomByteBlock` | `protocol_specs/sentry_bt_status_v1.json` | 官方命令字 `0x0310` 的 **4 字节**布局：`current_stance`(uint8, offset 0)、`bt_remain_time`(uint16 小端, offset 1–2)、`bt_game_status`(uint8, offset 3) |
| `RobotPathPlanInfo` | `robomaster.RobotPathPlanInfo` | 哨兵路径规划：`intention`、`start_pos_x/y`、`offset_x/y`（各 49 个 int32 增量）、`sender_id` |

`current_stance` 与 `bt_game_status` 的**取值语义（进攻/防守/撤退/待命等）尚未确认**，
因此只报告原始数值，不做业务解释。

### 本工具不支持全部 RM26 协议

`sim/robomaster_pb2.py` 里还有很多消息类型，但它们**没有**被注册，
因此收到时一律标记 `UNSUPPORTED`，只保存原文。这是刻意的边界：

- 不因为 pb2 里存在某个消息，就把它加进注册表；
- 不自动解析全部 `robomaster.proto`；
- 不在没有真实需求时提前实现协议。

### 新协议怎么加

只有同时满足以下三条才允许加入 `decoders.DECODERS`：

1. 出现**真实任务**，现场确实需要看这条消息；
2. 有**确定的协议定义**（字段、类型、字节序、长度/数量约束）；
3. 有**测试样例**（合法样例 + 至少一个反例）。

加入时只改 `decoders.py` 与对应测试，不动 `receiver.py`。

## 5. 常用运行命令

### 5.1 观察哨兵路径（`inspector`）

```bash
.venv-mqtt/bin/python -m tools.mqtt_protocol.inspector \
  --broker 127.0.0.1 \
  --port 3333 \
  --client-id mqtt-inspector-path \
  --topic RobotPathPlanInfo \
  --timeout 30 \
  --max-messages 1 \
  --expected-sender-id 7 \
  --dump-points \
  --allow-live-connect
```

### 5.2 观察所有 Topic，但只解析白名单

```bash
.venv-mqtt/bin/python -m tools.mqtt_protocol.inspector \
  --broker 127.0.0.1 \
  --port 3333 \
  --client-id mqtt-inspector-all \
  --topic '#' \
  --timeout 30 \
  --output tmp/mqtt_capture.jsonl \
  --allow-live-connect
```

参数说明：

| 参数 | 说明 |
| --- | --- |
| `--broker` / `--port` | Broker 主机与端口，必须显式指定；`--broker` 只填主机/IP，不带协议前缀 |
| `--client-id` | 独立 Client ID，代码与 JSON 中都不写死真实 ID |
| `--topic` | 要订阅的 Topic，可重复；默认 `#`（全部） |
| `--timeout` | 总超时秒数，默认 30 |
| `--max-messages` | 收到多少条消息后提前退出 |
| `--expected-sender-id` | `RobotPathPlanInfo` 期望的 `sender_id`，不匹配判 `NOT_MATCH` |
| `--dump-points` | 输出还原后的绝对路径点 |
| `--output` | 以 JSON Lines **追加**保存现场消息 |
| `--allow-live-connect` | 显式确认允许连接现场 Broker |

### 5.3 校验 `CustomByteBlock` 的 4 字节协议

```bash
.venv-mqtt/bin/python -m tools.mqtt_protocol.custom_byte_validator \
  --spec tools/mqtt_protocol/protocol_specs/sentry_bt_status_v1.json \
  --broker 127.0.0.1 \
  --port 3333 \
  --client-id sentry-validator-human \
  --timeout 15 \
  --max-matches 1 \
  --allow-live-connect
```

退出码：`0` 至少收到一个合法数据包；`2` 超时前未收到合法数据包；
`1` 环境或配置错误；`130` 被 Ctrl-C 中断。

### 5.4 测试

```bash
.venv-mqtt/bin/python -m unittest discover \
  -s tools/mqtt_protocol/tests -p 'test_*.py' -v
```

未装 protobuf 的解释器也能跑，但涉及真实编解码的用例会 skip，**不等于全部通过**。

## 6. 状态含义

| 状态 | 含义 |
| --- | --- |
| `PASS` | Topic 受支持，Protobuf 解析成功，且必要语义检查通过 |
| `NOT_MATCH` | Topic 受支持，Protobuf 解析成功，但字段/数量/取值/`sender_id` 不符合预期 |
| `PARSE_FAIL` | 收到了受支持 Topic，但无法解析为对应 Protobuf |
| `UNSUPPORTED` | 收到消息，但当前没有注册该 Topic 的解析器；已输出/保存原始信息 |
| `NO_MESSAGE` | 超时前没有收到目标消息（`inspector` 返回非零退出码） |

**语义检查失败不会丢弃数据**：`NOT_MATCH` 仍会输出已经解析成功的字段与路径点，
并在 `!` 行给出具体原因（例如"增量数量不是 49""第 12 个路径点越出场地边界"）。
`PARSE_FAIL` 才表示拿不到字段。

### 终端输出：先讲"能否解析"，再讲"校验结论"

终端把两件事分两行显示，避免把"解析失败"误读成"校验不过"：

```text
[2026-10-21 08:44:47.509] topic=RobotPathPlanInfo payload=110B -> PARSED
    validation=NOT_MATCH
    intention=4
    ! intention=4 不在 1/2/3 之内（1=到目标点攻击，2=到目标点防守，3=移动到目标点）
```

- 首行 `-> PARSED` 表示 **Topic 受支持且 Protobuf 解析成功**；
  解析失败显示 `-> PARSE_FAIL`，未注册 Topic 显示 `-> UNSUPPORTED`，
  **这两种都不会出现 `PARSED`**。
- `validation=...` 是**协议校验结论**：`PASS` 表示通过，
  `NOT_MATCH` 表示 **解析成功但不符合校验要求**（原因见其后的 `!` 行）。
- JSON Lines 里的 `status` 仍只取 `PASS` / `NOT_MATCH` / `PARSE_FAIL` / `UNSUPPORTED`，
  **不写 `PARSED`**——`PARSED` 只是终端展示用的标签，不改变数据文件结构。

## 7. 未支持 Topic 如何保存原始信息

收到未注册 Topic 时：

- 终端输出 `timestamp`、`topic`、`payload_length`，以及**有长度限制**的
  `payload_hex` 预览（前 32 字节，超出部分标注 `… (+NB)`），不刷屏；
- 标记 `UNSUPPORTED`，**不报致命错误**；
- 若给了 `--output`，以 JSON Lines 追加一行，至少包含：

```json
{"timestamp": "...", "topic": "...", "payload_length": 3,
 "payload_b64": "...", "status": "UNSUPPORTED", "fields": {}, "issues": []}
```

`payload_b64` 是完整原始载荷的 base64，因此即使当前不认识该 Topic，事后也能复现。
**本轮不实现回放**：回放留到真正需要时再做。

## 8. 本工具不做什么

- 不验证 `GameData`、不验证 QML、不验证 UI 渲染——那属于客户端测试；
- 不发布任何 MQTT 消息（只用 `subscribe`，代码中没有 `publish`）；
- 不修改官方协议字段，不新增协议语义；
- 不提供 GUI、数据库、后台服务或数据回放；
- 不解析白名单之外的 Topic。

## 9. 调试案例（`CustomByteBlock`）

| 输入（十六进制） | 来源 | 长度 | 结果 |
| --- | --- | --- | --- |
| `53 49 4D` | ASCII `SIM` | 3 字节 | `NOT_MATCH`（长度不等于 4） |
| `41 42 43 44` | ASCII `ABCD` | 4 字节 | `PASS` |

`ABCD`（`41 42 43 44`）的解析结果：

```text
current_stance = 65
bt_remain_time = 17218
bt_game_status = 68
```

其中 `bt_remain_time` 为小端 `0x4342` = 17218；若误按大端解释会得到 16963，
因此该案例也可用于确认字节序。

## 10. `RobotPathPlanInfo` 的检查规则

依据 2026 通信协议 V2.0.0：

| 检查 | 不通过时的状态 |
| --- | --- |
| `intention ∈ {1,2,3}`（1=到目标点攻击，2=到目标点防守，3=移动到目标点） | `NOT_MATCH` |
| `offset_x` 与 `offset_y` 数量一致 | `NOT_MATCH` |
| 两者各 49 个增量 | `NOT_MATCH` |
| 每个增量在 int8 范围 `[-128,127]` | `NOT_MATCH` |
| `start_pos_x ∈ [0,280] dm`、`start_pos_y ∈ [0,150] dm` | `NOT_MATCH` |
| 累计还原的路径点不越出场地边界 | `NOT_MATCH` |
| 给了 `--expected-sender-id` 时 `sender_id` 必须匹配 | `NOT_MATCH` |

路径点还原方式：**当前点 = 上一个点 + 当前增量**，起点为
`(start_pos_x, start_pos_y)`，因此 49 个增量会产生 50 个点（含起点）。

> MQTT 侧收到的是 Protobuf 字段，不是按官方结构体偏移直接解析，
> 因此这里**不**把 payload 长度写死成 103 或 105 字节。
