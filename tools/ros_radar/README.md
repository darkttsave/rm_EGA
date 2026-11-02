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

## ROS 2 Humble 环境

先安装并 source ROS 2 Humble 以及雷达同学提供的完整 `vision_interface` 消息包：

```bash
source /opt/ros/humble/setup.bash
source <vision_interface 工作空间>/install/setup.bash
ros2 interface show vision_interface/msg/RobotMapPositions
```

若消息包或 `rclpy` 不可用，程序会明确报错；离线测试仍可运行。

## 启动

持续查看解析结果和实时接收频率：

```bash
python3 -m tools.ros_radar.inspector
```

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
ros2 topic info /robot_map_positions --verbose
ros2 topic hz /robot_map_positions
python3 -m tools.ros_radar.inspector --timeout 30 --show-raw \
  --output tmp/ros_radar_capture.jsonl
```

验收时需要单独记录消息类型加载、节点启动、局域网持续接收、实测频率以及真实样本
解析结果。没有雷达与消息包时只能结论为“离线通过，真实 ROS 链路未验证”。
