# FGB FoxyFaceModule

[![build](https://github.com/Aqamoe/FGB-FoxyFaceModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-FoxyFaceModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。协议解析、映射与 ABI 生命周期有单元测试和合成 UDP
端到端冒烟；真机（FoxyFace 应用）验收待补，见 [docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 FoxyFace 原生模块。
直接接收 **FoxyFace**（摄像头面部追踪应用）按 VRCFT 线格式发到本机 25747 端口的 UDP JSON，
`Values` 的键名就是 Unified Expressions 形状名，按名写入表情，并输出睁眼度与逐眼注视。
不需要 VRCFT，不注入任何进程，也不需要管理员权限。

FoxyFace (VRCFT JSON over UDP) native input module for FaceGaze Bridge. Reads the
stream FoxyFace already sends to 127.0.0.1:25747 and outputs Unified Expressions
shapes, eye openness and per-eye gaze. No VRCFT, no process injection, no admin rights.

## 功能 / Features

- **表情**：键名直出 Unified Expressions（与 SDK 88 项同源），按名查表写入，不硬编码下标；
  未知键跳过并限频告警，绝不静默当成别的形状。
- **眼睛**：`EyeOpennessLeft/Right` 直接作为睁眼度；`EyeX*/EyeY*`（[-1,1] 投影分量）构造
  逐眼注视方向，极化未经真机确认，置信度 0.4。
- **心跳感知**：空闲时上游发 `{"PingPacket":true,...}` 心跳，模块识别后不误报断流、不产空帧。
- **断连可恢复**：10 秒没有数据时在模块卡片提示原因，数据恢复后自动回到活跃。
- **可诊断**：端口冲突、畸形 JSON、未知键名、非数字值都有明确日志，不静默丢数据。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 头姿 | 报文有 `HeadX/Y/Z`、`HeadPitch/Yaw/Roll`，但单位与范围未确认，先不接（见 docs/protocol.md） |
| 瞳孔 | 上游枚举里有 `EyePupilDiameterMM*`，但发送端映射不输出它 |
| 2D 注视点 | 报文没有该字段 |
| 渲染级注视 | `EyeX*/EyeY*` 是摄像头估计的近似量，只进 `eyes` 通道，不声明 `gaze` |

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） |
| 上游 | 安装 [FoxyFace](https://github.com/Jeka8833/FoxyFace) 并开启网络（VRCFT）输出，目标填本机 `127.0.0.1`（默认即 localhost:25747） |
| 其它 | 关闭 VRCFaceTracking：它和本模块抢同一个 UDP 端口 |

## 安装

1. 从 Releases 下载 `foxyface-0.1.0.zip`。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把 **FoxyFace** 设为启动，角色通常给 `expression` 与 `eyes`。
4. 重启引擎；FoxyFace 开始发送后模块卡片应显示活跃。

## 配置

模块目录下可选 `config.ini`：

```ini
port = 25747
```

改完重启引擎，并同步 FoxyFace 设置里的目标端口。未知键会被记录一条警告，不会静默失效。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。
本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="D:/code/FGB-Module/FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules   # build/Release/foxyface-<版本>.zip
ctest --test-dir build -C Release --output-on-failure           # 协议与映射单测
cmake --build build --config Release --target smoke-test        # 真实 ABI + 合成 UDP 冒烟（需 25747 空闲）
```

## 目录结构

```
src/main.cpp                 ABI 生命周期、UDP 接收、状态上报
src/foxyface_payload.*       JSON 解析与 Unified Expressions 写入
module/                      fgb-native.json 与图标
tests/                       协议单测、真实 ABI 冒烟
docs/protocol.md             线格式与映射依据（已确认/未确认逐条标注）
docs/verification.md         真机验收清单
cmake/                       依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 排错

| 现象 | 处理 |
|---|---|
| 卡片显示「无法绑定 UDP 25747：端口已被占用」 | 关闭 VRCFT（含 FoxyFace 的 VRCFT 接口插件）或其它接收端；或用 `config.ini` 换端口并同步改 FoxyFace 目标端口 |
| 一直提示「还没收到任何数据包」 | 确认 FoxyFace 网络输出已开启、目标地址是 127.0.0.1、防火墙没拦回环 UDP |
| 表情不动、眼睛正常 | 角色没分给 `expression` |
| 日志出现「无法识别的键名」 | 上游改了字段名；把版本号和日志一并提到 issue |
| 注视方向看起来反了 | 属已知风险：`EyeX*/EyeY*` 极化未做实机验证，欢迎带证据提 issue |

## 致谢 / Attribution

- 线格式依据来自上游 [Jeka8833/FoxyFace](https://github.com/Jeka8833/FoxyFace)
  （Python 应用 Apache-2.0；其 C# VRCFT 接口目录仅有 UNLICENSE）。本仓库是该线格式的
  独立 C++ 实现，未包含其源码。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
