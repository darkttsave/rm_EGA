## 启动

确认 AWAKENING 自瞄进程未运行，并在独立终端依次执行。

```bash
# 1. 构建 C++ Talos 读取助手（已验证）
cd /home/lzy/opensource/rm_AWAKENING-video
cmake -S . -B /tmp/awakening-video-build \
  -DBUILD_WITH_OPENVINO=OFF -DBUILD_WITH_TRT=OFF \
  -DBUILD_SENTRY=OFF -DBUILD_RADAR=OFF -DBUILD_WITH_RERUN=OFF \
  -DBUILD_RUNTIME_TESTS=OFF -DBUILD_WITH_PCH=OFF
cmake --build /tmp/awakening-video-build --target talos_rgb_pipe -j2

# 2. 启动 Daedalus（未完成本轮图形联调；默认 feature 已包含 talos）
cd /home/lzy/opensource/bevy_robomaster_simulator
cargo run --release

# 3. 启动 EGA 客户端（未完成本轮图形联调）
cd /home/lzy/opensource/RM26_Client-video
cmake --preset release
cmake --build --preset release
RM_VIDEO_PORT=3334 RM_VIDEO_STREAM_URL=udp://0.0.0.0:3334 \
  ./build/release/RoboMasterClient2025

# 4. 可选：只提供 MQTT 比赛状态，禁止模拟视频
./sim/run_sim.sh --no-video --mqtt-host 127.0.0.1 --mqtt-port 3333 \
  --current-robot-id 1

# 5. 启动图传桥接（已验证命令入口，完整联调待验收）
python3 tools/daedalus_video_bridge.py \
  --reader /home/lzy/opensource/rm_AWAKENING-video/bin/talos_rgb_pipe \
  --host 127.0.0.1 --port 3334 --fps 30 --reader-queue 2
```

## 参数

| 参数/配置 | 默认值 | 说明 |
| --- | --- | --- |
| `--reader` | `~/opensource/rm_AWAKENING-video/bin/talos_rgb_pipe` | C++ 读取助手 |
| `--host` / `--port` | `127.0.0.1` / `3334` | EGA 主图传目标 |
| `--fps` | `30` | 输出/编码目标帧率 |
| `--reader-queue` | `2` | C++ 有界队列，只允许 `1` 或 `2` |
| `--width` / `--height` | `1440` / `1080` | 必须与 Talos RGB24 图像一致 |
| `config.json`：`network.video_port` | `3334` | 与桥接端口一致 |
| `config.json`：`video.stream_url` | `udp://0.0.0.0:3334` | 客户端监听地址；环境变量可临时覆盖 |

## 验证

```bash
# 已验证：桥接单元与协议测试
cd /home/lzy/opensource/RM26_Client-video
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest sim.tests.test_video_bridge -v
```

Human 闭环验收：保持运行至少 60 秒；画面应连续、颜色和方向正确，无持续花屏；桥接 stderr 中 `pose_seq_mismatch`、`pose_missing` 不应持续增长，`queue` 不应持续积压。停止后重新执行桥接命令，客户端应恢复显示。

## 恢复

- 在桥接终端按 `Ctrl+C`；只会停止桥接创建的读取助手、FFmpeg 和 UDP socket，不会停止 Daedalus 或 EGA。
- 恢复自瞄测试前先停止桥接，再按 AWAKENING 原流程启动自瞄；二者不得同时消费 Talos 图像。
- 恢复 EGA 原模拟视频前先停止桥接，再执行 `./sim/run_sim.sh --video-only --video-file /绝对路径/视频.mp4`；同一时刻只允许一路视频发送到 `3334`。
- 如使用了环境变量覆盖，执行 `unset RM_VIDEO_PORT RM_VIDEO_STREAM_URL` 后重启客户端即可恢复 `config.json` 配置。
