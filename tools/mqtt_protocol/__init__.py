"""MQTT 监听与协议解析工具包。

分层约定（详见 README.md）：

- ``receiver``：只负责 MQTT 传输，不解析任何业务协议；
- ``decoders``：按 Topic 白名单解析具体 Protobuf 业务消息；
- ``inspector``：通用观察入口，把接收层与解码层拼起来；
- ``custom_byte_validator``：CustomByteBlock 的 4 字节协议校验器。

所有工具**只订阅，不发布**。
"""

__all__ = ["receiver", "decoders", "inspector", "custom_byte_validator"]
