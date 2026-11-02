#pragma once
// FoxyFace 的 VRCFT UDP JSON 负载解析与帧写入。
//
// 上游格式（FoxyFace 应用发到目标地址 127.0.0.1:25747 的一个数据报，UTF-8 JSON）：
//   {"Timestamp":<毫秒单调时钟>,"Config":{...},"Values":{"JawOpen":0.62,...}}
// 空闲超过 1 秒时改为心跳包：
//   {"PingPacket":true,"Config":{...}}
// Values 的键名就是 Unified Expressions 形状名（PascalCase，与 SDK 88 项同源），
// 另含自定义键 EyeOpennessLeft/Right、EyeX*/EyeY*（注视）与 Head*（头姿）。
// 详见 docs/protocol.md；未确认项在文档里逐条标注。

#include "fgb/kit/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace foxyface {

// 「键存在但没上报数值」与「上报了 0」必须区分：0 是合法值，缺失是不写。
inline constexpr float kNoValue = -32768.0f;

struct Packet {
    bool is_ping = false;
    std::uint64_t timestamp_ms = 0;
    // 已按 SDK 规范名归一的表情项（键名直出 UE，无需 ARKit 中转）。
    std::vector<std::pair<std::string, float>> shapes;
    float left_openness = kNoValue;
    float right_openness = kNoValue;
    float left_x = kNoValue;
    float left_y = kNoValue;
    float right_x = kNoValue;
    float right_y = kNoValue;
    std::size_t values_total = 0;
    std::size_t shapes_written = 0;
    std::size_t values_invalid = 0;  // 键认识但值不是数字
    std::size_t values_unknown = 0;  // 键不认识，必须跳过并计数
    std::size_t head_ignored = 0;    // Head* 已知但单位未确认，不写
};

// 解析一包 JSON。返回 false 表示这一包不可信（结构非法/超长），调用方应整包丢弃。
bool ParsePacket(std::string_view text, Packet& out, std::string& error);

// 登记本模块会写的全部 Unified Expressions 目标名（88 项全量，按名写入）。
void DeclareTargets(fgb::kit::ShapeTable& table);

// 写表情 + 双眼属性，并置上对应的 attributes 位。
void WriteFrame(const Packet& packet, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot);

} // namespace foxyface
