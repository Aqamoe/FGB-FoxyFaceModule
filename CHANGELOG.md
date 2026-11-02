# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- FoxyFace VRCFT 网络输出（UDP JSON，默认 127.0.0.1:25747）→ Unified Expressions 表情、
  睁眼度与逐眼注视；键名直出 UE，按名查 ShapeTable 写入，不硬编码下标。
- 心跳包（`PingPacket`）识别：空闲不误报断流、不产空帧。
- 未知键跳过并计数 + 限频告警；非数字值跳过；`Head*` 头姿键因单位未确认只计数不写。
- `config.ini` 支持覆盖端口。
- 协议单测 + 真实 ABI 端到端冒烟测试。

### 已知限制
- 未在真机（FoxyFace 应用）验证，状态为未验证；`EyeX*/EyeY*` 极化待实机确认。
