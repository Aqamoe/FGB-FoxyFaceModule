#include "foxyface_payload.hpp"

#include "fgb/kit/arkit.hpp"
#include "fgb/kit/json.hpp"
#include "fgb/kit/shape_names.hpp"

#include <cmath>
#include <string>

namespace foxyface {

namespace {

// Kit 的 JsonValue 是只读探测式的（不提供对象成员枚举），所以这里按
// 「协议声明的键名表逐项查值」解析：Unified Expressions 的 88 个规范名 +
// FoxyFace 自定义键。查不到的剩余键计入 values_unknown，绝不猜成别的形状。
//
// 精确 Find 先走（上游键名就是规范拼写），未命中再用大小写不敏感兜底，
// 兼容个别版本改成小驼峰的写法；兜底命中时写回的仍是 SDK 规范名。
const fgb::kit::JsonValue* Lookup(const fgb::kit::JsonValue& object, std::string_view name) {
    const fgb::kit::JsonValue* exact = object.Find(name);
    return exact != nullptr ? exact : object.FindInsensitive(name);
}

// 表项种类：表情形状 / 睁眼度 / 注视分量 / 头姿（已知但单位未确认）。
enum class KeyKind : std::uint8_t { Shape, OpennessL, OpennessR, GazeXL, GazeYL, GazeXR, GazeYR, Head };

struct ProbeKey {
    const char* name;
    KeyKind kind;
};

// FoxyFace 自定义键（不在 SDK 的 88 项 Unified Expressions 里）。
constexpr ProbeKey kCustomKeys[] = {
    {"EyeOpennessLeft", KeyKind::OpennessL},
    {"EyeOpennessRight", KeyKind::OpennessR},
    {"EyeXLeft", KeyKind::GazeXL},
    {"EyeYLeft", KeyKind::GazeYL},
    {"EyeXRight", KeyKind::GazeXR},
    {"EyeYRight", KeyKind::GazeYR},
    // 头姿键的单位与极化未在源码里确认（见 docs/protocol.md），先计数不写。
    {"HeadX", KeyKind::Head},
    {"HeadY", KeyKind::Head},
    {"HeadZ", KeyKind::Head},
    {"HeadPitch", KeyKind::Head},
    {"HeadYaw", KeyKind::Head},
    {"HeadRoll", KeyKind::Head},
};

float CleanSigned(float value) {
    if (std::isnan(value) || std::isinf(value)) {  // 上游偶尔发脏值，不能污染整帧
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value < -1.0f ? -1.0f : value;
}

double ToDouble(const fgb::kit::JsonValue& node) {
    return node.AsNumber(0.0);
}

void ApplyKey(const fgb::kit::JsonValue& node, KeyKind kind, Packet& out) {
    const float value = static_cast<float>(ToDouble(node));
    switch (kind) {
        case KeyKind::OpennessL:
            out.left_openness = fgb::kit::CleanUnit(value);
            break;
        case KeyKind::OpennessR:
            out.right_openness = fgb::kit::CleanUnit(value);
            break;
        case KeyKind::GazeXL:
            out.left_x = CleanSigned(value);
            break;
        case KeyKind::GazeYL:
            out.left_y = CleanSigned(value);
            break;
        case KeyKind::GazeXR:
            out.right_x = CleanSigned(value);
            break;
        case KeyKind::GazeYR:
            out.right_y = CleanSigned(value);
            break;
        case KeyKind::Head:
            ++out.head_ignored;
            break;
        case KeyKind::Shape:
            break;
    }
}

} // namespace

bool ParsePacket(std::string_view text, Packet& out, std::string& error) {
    const auto root = fgb::kit::ParseJson(text);
    if (!root.has_value()) {
        error = "JSON 解析失败";
        return false;
    }
    if (!root->IsObject()) {
        error = "顶层不是对象";
        return false;
    }

    out = Packet{};

    // 心跳包：连接活着但没有可写数据。识别它，让断流判定不误报。
    const fgb::kit::JsonValue* ping = Lookup(*root, "PingPacket");
    if (ping != nullptr && ping->AsBool(false)) {
        out.is_ping = true;
        return true;
    }

    const fgb::kit::JsonValue* timestamp = Lookup(*root, "Timestamp");
    if (timestamp != nullptr && timestamp->Kind() == fgb::kit::JsonKind::Number) {
        const double raw = ToDouble(*timestamp);
        out.timestamp_ms = raw > 0.0 ? static_cast<std::uint64_t>(raw) : 0;
    }

    const fgb::kit::JsonValue* values = Lookup(*root, "Values");
    if (values == nullptr || !values->IsObject()) {
        error = "缺少 Values 对象（且不是心跳包）";
        return false;
    }
    out.values_total = values->Size();
    if (out.values_total == 0) {
        error = "Values 为空";
        return false;
    }

    std::size_t matched = 0;

    // 表情：键名直出 Unified Expressions，按 SDK 规范名逐个探测后按名写入。
    for (std::size_t i = 0; i < fgb::kit::kUnifiedExpressionCount; ++i) {
        const std::string_view name = fgb::kit::kUnifiedExpressionNames[i];
        const fgb::kit::JsonValue* hit = Lookup(*values, name);
        if (hit == nullptr) {
            continue;
        }
        ++matched;
        if (hit->Kind() != fgb::kit::JsonKind::Number) {
            // 非数字值跳过：当成 0 会静默改掉一个形状，与「没上报」混淆。
            ++out.values_invalid;
            continue;
        }
        out.shapes.emplace_back(std::string{name}, fgb::kit::CleanUnit(static_cast<float>(ToDouble(*hit))));
        ++out.shapes_written;
    }

    for (const ProbeKey& key : kCustomKeys) {
        const fgb::kit::JsonValue* hit = Lookup(*values, key.name);
        if (hit == nullptr) {
            continue;
        }
        ++matched;
        if (hit->Kind() != fgb::kit::JsonKind::Number) {
            ++out.values_invalid;
            continue;
        }
        ApplyKey(*hit, key.kind, out);
    }

    if (values->Size() > matched) {
        out.values_unknown = values->Size() - matched;
    }

    if (out.shapes_written == 0 && out.values_unknown == 0 &&
        out.left_openness == kNoValue && out.right_openness == kNoValue &&
        out.left_x == kNoValue && out.left_y == kNoValue && out.right_x == kNoValue &&
        out.right_y == kNoValue && out.head_ignored == 0 && out.values_invalid == 0) {
        error = "Values 里没有认识的键";
        return false;
    }
    return true;
}

void DeclareTargets(fgb::kit::ShapeTable& table) {
    // 键名直出 UE：整表 88 项全部登记，写入时按名查 ShapeTable，不硬编码下标。
    for (std::size_t i = 0; i < fgb::kit::kUnifiedExpressionCount; ++i) {
        table.Declare(fgb::kit::kUnifiedExpressionNames[i]);
    }
}

void WriteFrame(const Packet& packet, const fgb::kit::ShapeTable& table, FgbFrameSlot* slot) {
    if (packet.is_ping) {
        return;
    }

    if (!packet.shapes.empty()) {
        slot->attributes |= FGB_ATTR_EXPRESSION;
        for (const auto& entry : packet.shapes) {
            table.Write(slot, entry.first, entry.second);
        }
    }

    if (packet.left_openness != kNoValue) {
        slot->attributes |= FGB_ATTR_LEFT_OPENNESS;
        slot->left_eye.openness = packet.left_openness;
        slot->left_eye.openness_confidence = 1.0f;
    }
    if (packet.right_openness != kNoValue) {
        slot->attributes |= FGB_ATTR_RIGHT_OPENNESS;
        slot->right_eye.openness = packet.right_openness;
        slot->right_eye.openness_confidence = 1.0f;
    }

    // 注视由 EyeX*/EyeY* 的 [-1,1] 投影分量构造（x 有界已在源码确认，极化方向
    // 未做实机验证，见 docs/protocol.md），置信度压低；x/y 必须成对才写。
    if (packet.left_x != kNoValue && packet.left_y != kNoValue) {
        slot->attributes |= FGB_ATTR_LEFT_GAZE;
        fgb::kit::GazeFromLookWeights(packet.left_x, packet.left_y, slot->left_eye.gaze_dir);
        slot->left_eye.gaze_confidence = 0.4f;
    }
    if (packet.right_x != kNoValue && packet.right_y != kNoValue) {
        slot->attributes |= FGB_ATTR_RIGHT_GAZE;
        fgb::kit::GazeFromLookWeights(packet.right_x, packet.right_y, slot->right_eye.gaze_dir);
        slot->right_eye.gaze_confidence = 0.4f;
    }
}

} // namespace foxyface
