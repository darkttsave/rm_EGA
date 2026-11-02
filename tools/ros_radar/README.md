# ROS 2 雷达位置检查器

只订阅 `/robot_map_positions`，按 `vision_interface/msg/RobotMapPositions` 解析每帧
完整快照，并在终端显示机器人原始像素坐标、世界坐标、雷达内部 ID 与接收频率。
工具不会发布消息，不做小地图坐标转换，也不把雷达 ID 映射为裁判系统 ID。

## 结构

```text
tools/ros_radar/
├── receiver.py   # ROS 节点、Reliable/Keep Last/Depth 10、接收统计
├── parser.py     # 纯 Python 字段解析与数据合法性校验
├── inspector.py  # 终端输出、快照变化、频率和 JSONL 记录
└── tests/        # 不需要 ROS 环境的离线单元测试
```

`robots=[]` 是合法的空快照。检查器会将上一帧仍存在、本帧已缺失的 `(color, id)`
显示为 `disappeared`；后续不会保留已经消失的目标。`in_bounds=false` 时保留越界坐标，
绝不钳制。

## 离线测试

在仓库根目录执行：

```bash
python3 -m unittest discover -s tools/ros_radar/tests -p 'test_*.py' -v
python3 -m tools.ros_radar.inspector --help
```

离线测试使用与 ROS 消息同字段的对象，只验证解析和检查器行为，不能代替真实链路验收。

## 启动

每个新终端都要先 source ROS 和消息工作空间。当前 Jazzy 开发机的完整启动命令：

```bash
source /opt/ros/jazzy/setup.bash
source "$HOME/vision_interface_ws/install/setup.bash"
cd "$HOME/opensource/RM26_Client-ros-radar"
python3 -m tools.ros_radar.inspector
```

Humble 验收机只需把第一行中的 `jazzy` 改为 `humble`，其余路径保持不变。

订阅创建成功后会立即看到：

```text
[ROS Radar] ROS 2 节点已启动：ros_radar_inspector
[ROS Radar] Topic：/robot_map_positions
[ROS Radar] 消息类型：vision_interface/msg/RobotMapPositions
[ROS Radar] QoS：Reliable、Keep Last、Depth 10
[ROS Radar] 订阅已创建（不代表已发现发布者）
[ROS Radar] 等待雷达消息...
```

“订阅已创建”只表示本地节点准备完毕，不表示已发现雷达发布者。首次收到消息时只输出
一次 `[ROS Radar] 已收到首帧消息，开始解析`，之后继续显示原有逐帧解析结果。

按 Ctrl+C 后会输出停止状态、运行时长、接收数、解析成功/失败数和程序退出状态。所有
生命周期日志都会立即刷新，不等待终端缓冲。

同时查看原始 ROS 字段并把原始消息、解析结果、出现/消失变化追加保存为 JSONL：

```bash
python3 -m tools.ros_radar.inspector \
  --show-raw \
  --output tmp/ros_radar_capture.jsonl
```

限制运行时间或消息数：

```bash
python3 -m tools.ros_radar.inspector --timeout 30 --max-messages 100
```

可用参数：

- `--topic`：默认 `/robot_map_positions`，必须是绝对 ROS topic。
- `--node-name`：默认 `ros_radar_inspector`。
- `--timeout`：运行秒数，省略时持续到 Ctrl-C。
- `--max-messages`：收到指定帧数后退出。
- `--show-raw`：终端额外输出未解释的原始字段。
- `--no-parsed`：不在终端逐帧输出解析结构。
- `--output`：追加记录 JSONL；记录始终同时包含原始字段与校验结论。

退出码：`0` 收到至少一帧，`2` 限时内没有消息，`1` 环境或参数错误，`130` 被
Ctrl-C 中断。

## 真实链路检查

在与雷达发布端 ROS 2 域配置一致的 Ubuntu 设备上，先确认发现和 QoS：

```bash
printenv ROS_DOMAIN_ID
ros2 topic list
ros2 topic info /robot_map_positions --verbose
ros2 topic hz /robot_map_positions
python3 -m tools.ros_radar.inspector --timeout 30 --show-raw \
  --output tmp/ros_radar_capture.jsonl
```

## ROS 2 与 `vision_interface` 环境

### 当前开发机状态

当前开发机是 Ubuntu 24.04 + ROS 2 Jazzy。雷达同学提供的完整消息包已整理并构建到：

```text
$HOME/vision_interface_ws
```

工作空间结构如下：

```text
vision_interface_ws/
├── src/
│   └── vision_interface/
│       ├── CMakeLists.txt
│       ├── package.xml
│       └── msg/
│           ├── RobotMapPosition.msg
│           ├── RobotMapPositions.msg
│           └── ...                 # 雷达包的其他消息定义
├── build/                  # Jazzy 构建产物
├── install/                # Jazzy 环境脚本和生成代码
└── log/                    # colcon 日志
```

正式工作空间的 `package.xml` 已补充 `ament_cmake_auto` 构建工具依赖。随压缩包附带的
嵌套空归档未复制进源码目录。

2026-11-02 在该工作空间完成的 Jazzy 验证：

- `colcon build --symlink-install --packages-select vision_interface` 通过；
- `colcon test` 及 `colcon test-result --verbose`：750 tests，0 errors，0 failures，
  236 skipped；
- `ros2 interface show vision_interface/msg/RobotMapPositions` 通过；
- Python 导入 `RobotMapPosition`、`RobotMapPositions` 通过；
- 检查器节点可启动；当时没有在线发布者，1 秒检查返回 `NO_MESSAGE`（退出码 2）。

### 在当前 Jazzy 开发机重新构建

如果终端提示符中有 `(.venv-mqtt)`，先执行 `deactivate`，或打开新终端。该虚拟环境只
服务 MQTT 工具，不能替代 ROS 2 的系统 Python 环境。

```bash
export VISION_WS="$HOME/vision_interface_ws"
test -f /opt/ros/jazzy/setup.bash
test -f "$VISION_WS/src/vision_interface/package.xml"

source /opt/ros/jazzy/setup.bash
cd "$VISION_WS"
colcon build --symlink-install --packages-select vision_interface
source "$VISION_WS/install/setup.bash"
ros2 interface show vision_interface/msg/RobotMapPositions
```

任一 `test -f` 返回非零都表示路径或环境有误，应停止并检查，不要继续执行。

### Humble 正式验收机

任务目标环境仍是 ROS 2 Humble。推荐使用 Ubuntu 22.04 + Humble，并将
`$HOME/vision_interface_ws/src/vision_interface` 源码复制到验收机同样的
`$HOME/vision_interface_ws/src/vision_interface`。不要复制 Jazzy 生成的 `build/`、
`install/` 或 `log/`；它们不能跨 ROS 发行版复用。

在 Humble 验收机执行：

```bash
export VISION_WS="$HOME/vision_interface_ws"
test -f /opt/ros/humble/setup.bash
test -f "$VISION_WS/src/vision_interface/package.xml"

source /opt/ros/humble/setup.bash
cd "$VISION_WS"
colcon build --symlink-install --packages-select vision_interface
source "$VISION_WS/install/setup.bash"
ros2 interface show vision_interface/msg/RobotMapPositions
```

若消息包或 `rclpy` 不可用，检查器会明确报错；离线测试仍可运行。


验收时需要单独记录消息类型加载、节点启动、局域网持续接收、实测频率以及真实样本
解析结果。当前已完成 Jazzy 消息包构建、类型加载和节点启动验证；尚未收到雷达发布的
真实 `/robot_map_positions`，因此局域网真实链路仍未通过验收。
