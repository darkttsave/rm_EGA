# MQTT 自定义协议监听校验器

独立监听 MQTT `CustomByteBlock`，解包其中的 Protobuf，并按协议 JSON 描述检查载荷内容。

本工具**只订阅、只解析**，不向 Broker 发布任何消息，也不改变现有客户端链路；
它用于在电控/客户端联调时独立观察"发出的字节到底是什么"，而不是替代客户端解析。

固件侧真实链路仍为：哨兵 NUC → C 板 → 官方控制器/裁判系统 → 选手端 → RM26_Client。
本工具只观察链路末端的 MQTT 报文，不代表官方链路已经验证通过。

---

## 1. 工具用途

1. 独立监听 MQTT Topic `CustomByteBlock`；
2. 将 payload 解包为 `robomaster.CustomByteBlock`，取其中的 `data` 字节；
3. 按协议 JSON 描述校验长度、解析字段并输出原始数值；
4. 不发布数据、不修改现有链路、不写入任何状态。

## 2. 当前包含的协议描述

```text
tools/protocol_specs/sentry_bt_status_v1.json
```

该 JSON 是当前唯一的协议描述来源，记录协议名称/版本、MQTT Topic、
Protobuf 消息类型、官方来源命令字 `0x0310`、`payload_length`、字段名、
offset、类型与字节序。Python 代码中不写死长度与字段名。

JSON 只用于本地描述协议，**不作为 MQTT payload 发送**。

## 3. 当前人工验证过的载荷布局

| 字段 | offset | 类型 | 字节序 |
| --- | --- | --- | --- |
| `current_stance` | 0 | uint8 | — |
| `bt_remain_time` | 1–2 | uint16 | 小端 |
| `bt_game_status` | 3 | uint8 | — |

总长度 **4 字节**（`payload_length = 4`）。

`current_stance` 与 `bt_game_status` 的**取值语义（进攻/防守/撤退/待命等）尚未确认**，
因此 JSON 与本工具都只保留原始数值，不做任何业务解释。

## 4. 环境准备与运行

本工具依赖两个第三方包，且版本都有明确约束；下面给出从零开始的完整步骤。

### 4.1 依赖与版本要求

| 依赖 | 版本要求 | 为什么是这个范围 |
| --- | --- | --- |
| Python | ≥ 3.11 | 与 `sim/pyproject.toml` 的 `requires-python` 一致 |
| `paho-mqtt` | ≥ 2.0 | 代码使用 `CallbackAPIVersion.VERSION2`，该 API 由 paho 2.0 引入 |
| `protobuf` | ≥ 6.33.2, < 7 | `sim/robomaster_pb2.py` 的 gencode 为 6.33.2，导入时会自校验运行时版本 |

`protobuf` 的范围与 `sim/requirements.txt`、`sim/pyproject.toml` 保持一致（同一口径，
由 `tools/release/check_sim_protobuf_runtime.py` 从 pb2 生成代码推导）。

**不需要** Qt、系统 `protoc` 或 ROS：本工具只用到 Python 标准库加上述两个包。

### 4.2 用哪个解释器

两条等价路径，按手头情况选一条：

- **A. 复用模拟器 venv**：若已按 [`sim/README.md`](../sim/README.md) 建好 `sim/.venv`，
  它已包含 `paho-mqtt 2.1.0` 与 `protobuf 6.33.6`，满足要求，直接用
  `sim/.venv/bin/python` 即可，不需要再安装任何东西。
- **B. 专用 venv `.venv-mqtt`**：只想跑本工具时更轻量，步骤见 4.3 / 4.4。

`.venv-mqtt/` 与 `sim/.venv/` 都被 `.gitignore` 忽略，**不会随 clone 出现**，
换机器后必须重新创建。

### 4.3 创建虚拟环境（路径 B）

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

### 4.4 安装依赖（路径 B）

```bash
python -m pip install 'paho-mqtt>=2.0' 'protobuf>=6.33.2,<7'
```

`<7` 的上限不是随手写的：`sim/robomaster_pb2.py` 导入时会调用
`ValidateProtobufRuntimeVersion`，主版本不符会直接失败。安装后即得到验证过的组合
（`paho-mqtt 2.1.0` + `protobuf 6.33.2`）。

### 4.5 自检

```bash
python -c "import paho.mqtt, google.protobuf as g; print('paho', paho.mqtt.__version__); print('protobuf', g.__version__); print(g.__file__)"
python tools/mqtt_custom_protocol_validator.py --help
```

期望输出类似（路径随你的 venv 位置而变）：

```text
paho 2.1.0
protobuf 6.33.2
/<仓库路径>/.venv-mqtt/lib/python3.12/site-packages/google/protobuf/__init__.py
```

- `--help` 不导入任何第三方依赖（依赖是延迟导入），裸解释器也能执行，可用来判断脚本本身是否完好；
- 若 `g.__file__` 不落在你所选的 venv 内，说明 `PYTHONPATH` 上有别的 `protobuf` 抢先
  （`PYTHONPATH` 在 `sys.path` 中先于 site-packages）。本机 ROS 的 `PYTHONPATH` 不含
  `google` / `paho`，因此不冲突；换机器时用这条命令确认最稳妥。

### 4.6 运行命令示例

连接现场 Broker 必须显式给出 `--broker`、`--port`、`--client-id` 与
`--allow-live-connect`；缺少 `--allow-live-connect` 时会拒绝连接并提示
"相同 Client ID 可能挤掉正在运行的客户端"。

```bash
.venv-mqtt/bin/python tools/mqtt_custom_protocol_validator.py \
  --spec tools/protocol_specs/sentry_bt_status_v1.json \
  --broker 127.0.0.1 \
  --port 3333 \
  --client-id <由操作者指定，勿使用在线客户端的 ID> \
  --timeout 15 \
  --max-matches 1 \
  --allow-live-connect
```

把开头的解释器换成你实际选用的那个（例如 `sim/.venv/bin/python`，或激活 venv 后直接用 `python`）。

### 4.7 参数说明

| 参数 | 说明 |
| --- | --- |
| `--spec` | 协议描述 JSON 路径，默认即上表的 sentry 协议 |
| `--broker` / `--port` | Broker 主机与端口，必须显式指定；`--broker` 只填主机/IP，不带协议前缀 |
| `--client-id` | 本次校验使用的 Client ID，代码与 JSON 中都不写死真实 ID |
| `--allow-live-connect` | 显式确认允许连接现场 Broker |
| `--timeout` | 总超时秒数（含端口预检），默认 30 |
| `--max-matches` | 收到多少个合法数据包后提前退出，便于一次性验证 |
| `--rate-window` | 频率统计窗口秒数，默认 1 |
| `--max-rate-hz` | 频率告警上限，默认取 JSON 中的 `max_rate_hz`（50Hz） |

### 4.8 退出码

`0` 至少收到一个合法数据包；`2` 超时前未收到合法数据包；`1` 环境或配置错误；
`130` 被 Ctrl-C 中断。

### 4.9 依赖缺失或版本不符

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `错误：无法导入 .../sim/robomaster_pb2.py：No module named 'google'`，并提示 `protobuf>=6.33.2,<7` | 用了没装依赖的系统 `python3` | 改用 4.2 选定的 venv 解释器 |
| `错误：未安装 paho-mqtt，无法连接 broker` | 缺 `paho-mqtt`；**只在真正连接时**才暴露，解析与 `--help` 不受影响 | 按 4.4 安装 |
| protobuf 版本不符（过旧或 7.x） | 未按范围安装 | 按 `>=6.33.2,<7` 重装 |
| `.venv-mqtt/bin/python: No such file or directory` | venv 未创建（被 gitignore，不随 clone） | 按 4.3 创建 |

用未装依赖的系统 `python3` 跑测试会得到 `Ran 41 tests … OK (skipped=1)`：涉及真实
Protobuf 编解码的用例会被跳过，**这不是全部通过**。要完整验证必须用装好依赖的解释器：

```bash
.venv-mqtt/bin/python -m unittest tools.tests.test_mqtt_custom_protocol_validator
```

期望结果是 `Ran 52 tests … OK`。

## 5. PASS、NOT_MATCH、FAIL 的含义

| 结果 | 触发条件 | 含义 |
| --- | --- | --- |
| `PASS` | Protobuf 解析成功，且 `data` 长度等于协议 `payload_length` | 载荷符合当前协议描述，随后输出各字段原始值 |
| `NOT_MATCH` | Protobuf 解析成功，但 `data` 长度不等于协议 `payload_length` | 该报文不属于当前协议；`CustomByteBlock` 是共享 Topic，视频等其它数据会走到这里 |
| `FAIL` | Protobuf 本身无法解析 | 报文不是合法的 `CustomByteBlock`；不会导致程序崩溃 |

只有 `PASS` 计入匹配数量与频率统计。`NOT_MATCH` 与 `FAIL` 的逐条日志会限流
（前若干条详细输出，之后按条数汇总），避免共享 Topic 上的高频数据刷屏。

频率按匹配数据包计算，超过协议上限（默认 50Hz）时给出警告。

## 6. 能力边界

- 协议描述文件可以更换，`--spec` 指向哪份 JSON 就按哪份解析；
- 当前只有 `sentry_bt_status_v1` 这一套 4 字节协议经过自动测试与人工验证；
- 对当前协议而言，非 4 字节载荷会显示 `NOT_MATCH`；
- **更换 JSON 不等于工具已经支持并验证任意字段类型、长度和复杂协议**；
- 新协议需要先确认解码器支持的字段类型与长度范围，并补充相应测试；
- 本工具不判断业务语义，也不判断"这个值是否合理"，只报告字节层面的解析结果。

## 7. 调试案例

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
