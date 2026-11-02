#include "foxyface_payload.hpp"

#include "fgb/kit/module.hpp"
#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <string>

using namespace fgb::kit;
using foxyface::Packet;
using foxyface::ParsePacket;

namespace {

const char* kBasicPayload =
    R"({"Timestamp":1700000000123,"Config":{"Name":"FoxyFace","Author":"Jeka8833","Type":"face"},
        "Values":{"JawOpen":0.62,"BrowInnerUpLeft":0.8,"MouthCornerPullLeft":0.4,
                  "EyeSquintLeft":0.2,"TongueOut":1.0,
                  "EyeOpennessLeft":0.75,"EyeOpennessRight":0.9,
                  "EyeXLeft":-0.25,"EyeYLeft":0.1,"EyeXRight":0.3,"EyeYRight":-0.05,
                  "HeadYaw":0.4,"HeadPitch":-0.2}})";

FgbFrameSlot FreshSlot() {
    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.abi_version = FGB_MODULE_ABI_V2;
    slot.shape_count = FGB_MAX_SHAPES;
    return slot;
}

// shapes 按 SDK 表序存放（不是包内顺序），按名取值。
// 用具名函数而不是 lambda：MSVC 14.36 在「捕获数组 + lambda 内下标」上有 ICE 记录。
float ShapeValue(const Packet& packet, const char* name) {
    for (const auto& entry : packet.shapes) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return -1.0f;
}

} // namespace

int main() {
    // ---- 正常键值：键名直出 Unified Expressions，按名写表 ----
    Packet packet{};
    std::string error;
    CHECK(ParsePacket(kBasicPayload, packet, error));
    CHECK(!packet.is_ping);
    CHECK_EQ(packet.timestamp_ms, static_cast<std::uint64_t>(1700000000123));
    CHECK_EQ(packet.shapes_written, static_cast<std::size_t>(5));
    CHECK_EQ(packet.values_total, static_cast<std::size_t>(13));
    CHECK_EQ(packet.values_unknown, static_cast<std::size_t>(0));
    CHECK_EQ(packet.head_ignored, static_cast<std::size_t>(2));  // Head* 计数但不写
    CHECK_NEAR(packet.left_openness, 0.75, 1e-6);
    CHECK_NEAR(packet.right_openness, 0.9, 1e-6);
    CHECK_NEAR(packet.left_x, -0.25, 1e-6);
    CHECK_NEAR(packet.right_y, -0.05, 1e-6);

    // ---- 未知键：必须忽略并计数，绝不静默当成别的形状 ----
    Packet unknown{};
    CHECK(ParsePacket(R"({"Values":{"JawOpen":0.5,"BrandNewTrackerKey":0.3,"AnotherOne":0.1}})",
                      unknown, error));
    CHECK_EQ(unknown.shapes_written, static_cast<std::size_t>(1));
    CHECK_EQ(unknown.values_unknown, static_cast<std::size_t>(2));
    // Head* 与未知键是两类：头姿键认识但不接，不进 unknown。

    // ---- 大小写差异：兜底命中后写回 SDK 规范名 ----
    Packet cased{};
    CHECK(ParsePacket(R"({"Values":{"jawOPEN":0.5,"EYEOPENNESSLEFT":0.4,"EyeXright":0.2}})",
                      cased, error));
    CHECK_EQ(cased.shapes_written, static_cast<std::size_t>(1));
    CHECK(cased.shapes[0].first == "JawOpen");  // 规范名，不是包里的拼写
    CHECK_NEAR(cased.left_openness, 0.4, 1e-6);
    CHECK_NEAR(cased.right_x, 0.2, 1e-6);
    // Timestamp/Config/Values 顶层字段同样容忍小写。
    Packet lowercase_top{};
    CHECK(ParsePacket(R"({"timestamp":42,"values":{"JawOpen":0.25}})", lowercase_top, error));
    CHECK_EQ(lowercase_top.timestamp_ms, static_cast<std::uint64_t>(42));
    CHECK_EQ(lowercase_top.shapes_written, static_cast<std::size_t>(1));

    // ---- 非数字值：跳过并计数，不能当成 0 ----
    Packet invalid{};
    CHECK(ParsePacket(R"({"Values":{"JawOpen":"0.5","JawLeft":null,"TongueOut":true,
                                    "JawRight":0.4}})", invalid, error));
    CHECK_EQ(invalid.values_invalid, static_cast<std::size_t>(3));
    CHECK_EQ(invalid.shapes_written, static_cast<std::size_t>(1));

    // ---- 心跳包：识别为 ping，不产帧 ----
    Packet ping{};
    CHECK(ParsePacket(R"({"PingPacket":true,"Config":{"Name":"FoxyFace"}})", ping, error));
    CHECK(ping.is_ping);

    // ---- 畸形 / 超大输入必须整包丢弃 ----
    Packet broken{};
    CHECK(!ParsePacket("not json", broken, error));
    CHECK(!ParsePacket(R"([1,2,3])", broken, error));
    CHECK(!ParsePacket(R"({"Config":{}})", broken, error));           // 缺 Values 且非心跳
    CHECK(!ParsePacket(R"({"Values":{}})", broken, error));           // 空 Values
    CHECK(!ParsePacket(R"({"Values":[{"k":"JawOpen","v":1}]})", broken, error));  // Values 不是对象
    CHECK(!ParsePacket(std::string("{\"Values\":{\"JawOpen\":") + std::string(20000, '9') + "}}",
                       broken, error));                               // 数值溢出（双精度放不下）
    CHECK(!ParsePacket(std::string(100000, '['), broken, error));     // 深度超限的超大报文
    CHECK(!ParsePacket(R"({"Values":{"JawOpen":1e400}})", broken, error));

    // ---- NaN/Inf/越界清洗：1e300 转 float 是 Inf，按 0 处理；越界裁剪 ----
    Packet dirty{};
    CHECK(ParsePacket(R"({"Values":{"JawOpen":1e300,"JawLeft":-0.5,"BrowInnerUpLeft":2.5}})",
                      dirty, error));
    CHECK_EQ(dirty.shapes_written, static_cast<std::size_t>(3));
    CHECK_NEAR(ShapeValue(dirty, "JawOpen"), 0.0, 1e-9);           // Inf → 0
    CHECK_NEAR(ShapeValue(dirty, "JawLeft"), 0.0, 1e-9);           // 负值 → 0
    CHECK_NEAR(ShapeValue(dirty, "BrowInnerUpLeft"), 1.0, 1e-9);   // >1 → 1
    CHECK_NEAR(ShapeValue(dirty, "JawRight"), -1.0, 1e-9);         // 没上报的键不出现
    Packet dirty_eye{};
    CHECK(ParsePacket(R"({"Values":{"EyeXLeft":9.0,"EyeYLeft":-9.0,"EyeOpennessLeft":1e300}})",
                      dirty_eye, error));
    CHECK_NEAR(dirty_eye.left_x, 1.0, 1e-9);         // 注视分量截断到 [-1,1]
    CHECK_NEAR(dirty_eye.left_y, -1.0, 1e-9);
    CHECK_NEAR(dirty_eye.left_openness, 0.0, 1e-9);

    // ---- 帧写入：ShapeTable 按名写入 + 眼睛属性 ----
    testing::HostMock host;
    Host logger{host.services()};
    ShapeTable table;
    foxyface::DeclareTargets(table);
    CHECK(table.Build(logger));
    CHECK_EQ(table.ResolvedCount(), table.Size());  // 88 项目标名必须都是真实形状

    FgbFrameSlot slot = FreshSlot();
    foxyface::WriteFrame(packet, table, &slot);

    CHECK((slot.attributes & FGB_ATTR_EXPRESSION) != 0);
    CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
    CHECK((slot.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
    CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
    CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
    CHECK((slot.attributes & FGB_ATTR_HEAD) == 0);  // 头姿未确认，不声明

    CHECK_NEAR(slot.shapes[host.IndexOf("JawOpen")], 0.62, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("BrowInnerUpLeft")], 0.8, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("MouthCornerPullLeft")], 0.4, 1e-6);
    CHECK_NEAR(slot.shapes[host.IndexOf("TongueOut")], 1.0, 1e-6);
    CHECK(slot.shape_valid[host.IndexOf("JawLeft")] == 0);  // 本包没有 JawLeft

    CHECK_NEAR(slot.left_eye.openness, 0.75, 1e-6);
    CHECK_NEAR(slot.right_eye.openness, 0.9, 1e-6);
    CHECK_NEAR(slot.left_eye.gaze_confidence, 0.4, 1e-6);

    // 注视方向必须是单位向量，且 -Z 前。
    const float length = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                   slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                   slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
    CHECK_NEAR(length, 1.0, 1e-5);
    CHECK(slot.left_eye.gaze_dir[2] < 0.0f);
    CHECK(slot.left_eye.gaze_dir[0] < 0.0f);  // EyeXLeft 为负 → x 分量为负
    CHECK(slot.left_eye.gaze_dir[1] > 0.0f);  // EyeYLeft 为正 → y 分量为正

    // 心跳包不写任何属性。
    FgbFrameSlot ping_slot = FreshSlot();
    Packet ping_packet{};
    ping_packet.is_ping = true;
    foxyface::WriteFrame(ping_packet, table, &ping_slot);
    CHECK_EQ(ping_slot.attributes, static_cast<std::uint64_t>(0));

    // ---- 断流判定：超时只报一次错误，心跳/数据恢复后回到活跃 ----
    testing::HostMock link_host;
    Host link_logger{link_host.services()};
    Staleness staleness{10ull * 1000000000ull, "FoxyFace"};
    const std::uint64_t second = 1000000000ull;
    CHECK(!staleness.Check(link_logger, 0, 5 * second));       // 还没收到任何包 → 报一次
    CHECK(!staleness.Check(link_logger, 0, 6 * second));       // 同一状态不重复报
    CHECK_EQ(link_host.CountStatuses(FGB_STATUS_ERROR), static_cast<std::size_t>(1));
    CHECK(staleness.Check(link_logger, 6 * second, 6 * second));      // 首包到达 → 活跃
    CHECK(!staleness.Check(link_logger, 6 * second, 20 * second));    // 静默 14 秒 → 断流
    CHECK(!staleness.Check(link_logger, 6 * second, 25 * second));    // 同一次断流只报一次
    CHECK_EQ(link_host.CountStatuses(FGB_STATUS_ERROR), static_cast<std::size_t>(2));
    CHECK(staleness.Check(link_logger, 26 * second, 26 * second));    // 恢复（心跳包也算收包）
    CHECK_EQ(link_host.CountStatuses(FGB_STATUS_ACTIVE), static_cast<std::size_t>(2));
    CHECK(link_host.LastStatusDetail().find("FoxyFace") != std::string::npos);

    return CheckSummary("foxyface-payload");
}
