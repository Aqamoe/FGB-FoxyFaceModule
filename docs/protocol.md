# FoxyFace VRCFT 网络输出协议要点

来源：上游仓库 [Jeka8833/FoxyFace](https://github.com/Jeka8833/FoxyFace)（main 分支，2026-11-02
经 GitHub API 逐文件核对）。发送端为 Python 应用（`FoxyFace/` 目录，Apache-2.0）；
`FoxyFaceVRCFTInterface/` 是给 VRCFT 宿主用的 C# 接收插件（该目录只有 `UNLICENSE` 文件，
未读到正文，按「无标准许可证」对待）。本模块是线格式的独立 C++ 实现，**未复制上游任何源码**，
只参考字段名、端口与结构。

## 端口与连接方式

| 事实 | 状态 | 依据 |
|---|---|---|
| 默认目标 `localhost:25747`（ip/port 可在 FoxyFace 设置改） | **已确认** | `config/schemas/core/SocketConfig.py`：`ip="localhost"`、`port=25747` |
| 发送端是 FoxyFace（UDP sendto），本模块是接收端 | **已确认** | `stream/vrcft/VRCFTUdpSocket.py`：循环 `sock.sendto(data, target_address)` |
| `57130` 自动发现：FoxyFace **监听** 57130 上的广播，广播方（VRCFT 宿主）地址+`{"Port":N}` 会覆盖它的发送目标 | **已确认**（方向）；FGB 宿主不广播，**未确认** | `stream/vrcft/VrcftAutoConnect.py`：`bind(("",57130))`、`recvfrom` 后写 `socket.ip/port`。因此必须手动把目标填成 `127.0.0.1` |
| 空闲 >1s 发送心跳包 | **已确认** | `VRCFTUdpSocket.py` `ping_connection_time=1.0` + `VrcftPacketEncoderStream.generate_ping_packet()` |

## 报文结构

每个 UDP 数据报是**一个** JSON（`json.dumps(..., separators=(",",":")` 后一次 `sendto`），
数据报内不会拼接多条 JSON —— **已确认**（发送端一次一包）。C# 接收端是否有拼包兜底**未确认**，
对本模块无影响。UTF-8，无分帧头。

数据包：

```json
{
  "Timestamp": 1700000000123,
  "Config": { "Name": "...", "Author": "...", "Type": "face", "Parameters": [ ... ] },
  "Values": { "JawOpen": 0.62, "EyeOpennessLeft": 0.75, "HeadYaw": 0.4 }
}
```

心跳包（无数据时的空闲发送）：

```json
{ "PingPacket": true, "Config": { ... } }
```

| 字段 | 用法 |
|---|---|
| `Timestamp` | **已确认**为 `perf_counter_ns()//1e6`，单调毫秒，不是 epoch；本模块只记录，断流判定以收包时刻为准 |
| `Config` | **未确认细节**（`VrcftInterfaceOptions.to_packet_format_dict()` 未读到）；本模块不依赖它 |
| `Values` | 键=形状名、值=数字；**值为 None 的项不会出现在包里**（上游过滤）→ 缺键 ≠ 0，本模块不写该形状 |
| 心跳包 | 识别后不产帧，但计入「链路活着」，不误报断流 |

## Values 键名集合（大小写）

上游 `stream/vrcft/UnifiedExpressionEnum.py` 是 `StrEnum` 且**值与名完全一致**（PascalCase），
`stream/vrcft/VrcftPacketEncoderStream.py` 直接以该枚举为键序列化 —— 即 **JSON 键名就是
Unified Expressions 规范名**，与 SDK 的 88 项 `kUnifiedExpressionNames` 同源。

发送端实际能输出的键（已逐个在 encoder 映射里确认）：

- 表情（UE 标准名）：`BrowLowerer*`、`BrowPinch*`、`BrowInnerUp*`、`BrowOuterUp*`、
  `CheekPuff*`、`CheekSquint*`、`CheekSuck*`、`EyeSquint*`、`EyeWide*`、`JawForward/Left/Open/Right`、
  `MouthClosed`、`MouthDimple*`、`MouthFrown*`、`LipFunnel*`、`LipPucker*`、`LipSuck*`、
  `MouthUpperLeft/Right`、`MouthLowerLeft/Right`、`MouthLowerDown*`、`MouthPress*`、
  `MouthRaiserUpper/Lower`、`MouthCornerPull*`、`MouthCornerSlant*`、`MouthStretch*`、
  `MouthUpperUp*`、`NoseSneer*`、`TongueOut/Up/Down/Left/Right/Roll/BendDown/CurlUp/Squish/Flat/Twist*`。
- 自定义眼睛键（**不在** SDK 88 项里）：`EyeOpennessLeft/Right`、`EyeXLeft/EyeYLeft/EyeXRight/EyeYRight`。
- 头姿键（同样不在 88 项里）：`HeadX/Y/Z`、`HeadPitch/Yaw/Roll`。

枚举里还有 `NasalDilation*`、`JawBackward`、`JawClench`、`EyePupilDiameterMM*` 等，但 encoder
映射未输出它们（瞳孔没有数据源）——视为不发送。

本模块解析按 SDK 规范名精确查找，未命中再大小写不敏感兜底；未知键**跳过并计数、限频告警**，
绝不静默映射到别的形状。

## 值范围

| 键 | 范围 | 状态 |
|---|---|---|
| 表情系数 | 源是 MediaPipe/Babble blendshape（0..1），且 `json.dumps(allow_nan=False)` 保证不出 NaN/Inf | **已确认**（编码端）；标定后是否可能越界 **未确认** → 模块按 [0,1] 裁剪 |
| `EyeOpenness*` | `1 - eyeBlink`，[0,1] | **已确认**（encoder 公式） |
| `EyeX*/EyeY*` | `GeneralBlendShapeEnum` 中 `min_value=-1.0, has_center=True`；上限/默认值未读到 | **部分确认**；模块按 [-1,1] 截断 |
| `Head*` | 单位（米/弧度?）与范围**未确认**（枚举定义在文件尾部，未核对） | **未确认** → 不接入，计数后忽略 |
| 极化（左右、x/y 符号） | 未经真机确认 | **未确认** → 注视置信度压低（0.4），文档如实标注 |

## 本模块的使用口径

- 表情：按名写 `ShapeTable`（88 项全量声明，不硬编码下标）。
- 眼睛：`EyeOpenness*` → 睁眼度（置信度 1.0）；`EyeX*/EyeY*` → 逐眼注视方向
  （投影法向量化，置信度 0.4，x/y 成对才写）。
- 不提供：头姿（单位未确认）、瞳孔（上游不发送）、2D 注视点、渲染级 `gaze` 通道。

## 冲突

同一端口只能有一个接收者。VRCFT 宿主（含 `FoxyFaceVRCFTInterface` C# 插件）和 VSeeFace 等
都可能占 25747；绑定失败时模块卡片给出端口与原因。改端口在模块目录 `config.ini`：

```ini
port = 25748
```

改完记得同步 FoxyFace 设置里的目标端口。
