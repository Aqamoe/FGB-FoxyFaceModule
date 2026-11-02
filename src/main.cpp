// FoxyFace → FaceGaze Bridge 原生模块。
//
// 监听 FoxyFace 按 VRCFT 线格式发到本机 25747 端口的 UDP JSON（默认目标就是
// localhost:25747），Values 的键名即 Unified Expressions 形状名，按名写入表情，
// 并输出睁眼度与逐眼注视。不需要 VRCFT，不注入任何进程，也不需要管理员权限。

#include "foxyface_payload.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"

#include <cstdint>
#include <string>

namespace {

constexpr std::uint16_t kDefaultPort = 25747;
constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;

class FoxyFaceSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        config.AllowKeys({"port"});
        if (config.Load(directory, "config.ini", config_error)) {
            port_ = static_cast<std::uint16_t>(config.GetInt("port", port_));
            if (!config_error.empty()) {
                host.Warn("config.ini：" + config_error);
            }
            if (!config.UnknownKeys().empty()) {
                host.Warn("config.ini 有未识别的键：" + config.UnknownKeys()[0]);
            }
        }

        fgb::kit::ShapeTable table;
        foxyface::DeclareTargets(table);
        if (!table.Build(host)) {
            error = "宿主没有提供 shape_index，无法映射 Unified Expressions";
            return false;
        }
        if (table.ResolvedCount() < table.Size()) {
            host.Warn("部分 Unified Expressions 形状未被宿主识别，已跳过 " +
                      std::to_string(table.Size() - table.ResolvedCount()) + " 个");
        }
        table_ = std::move(table);

        if (!endpoint_.Bind(port_, error)) {
            return false;
        }
        host.Info("FoxyFace 监听 UDP " + std::to_string(port_) +
                  "，请在 FoxyFace 的网络输出里把目标地址设为 127.0.0.1:" +
                  std::to_string(port_));
        host.StatusStarting("等待 FoxyFace 数据");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        foxyface::Packet packet;
        bool parsed = false;

        fgb::kit::UdpPacket datagram;
        // FoxyFace 按帧率推送，只处理最新一包；积压的旧包丢掉比逐帧回放更准。
        if (endpoint_.PollLatest(datagram)) {
            const std::string_view text{reinterpret_cast<const char*>(datagram.data.data()),
                                        datagram.data.size()};
            std::string error;
            if (foxyface::ParsePacket(text, packet, error)) {
                if (packet.is_ping) {
                    // 心跳只证明链路活着；不产帧，断流判定照常按收包时刻算。
                    host.Trace("收到 FoxyFace 心跳包（上游空闲，暂无表情数据）");
                } else {
                    parsed = true;
                    // 未知键 / 非法值限频或去重告警，不刷屏，也绝不静默改写成别的形状。
                    if (packet.values_unknown > 0 && packet.values_unknown != reported_unknown_) {
                        reported_unknown_ = packet.values_unknown;
                        host.Warn("FoxyFace 上报了 " + std::to_string(packet.values_unknown) +
                                  " 个无法识别的键名（模块会忽略它们，不会当成别的形状）");
                    }
                    if (packet.values_invalid > 0 && packet.values_invalid != reported_invalid_) {
                        reported_invalid_ = packet.values_invalid;
                        host.Warn("本包有 " + std::to_string(packet.values_invalid) +
                                  " 个键的值不是数字，已跳过（缺值与 0 不是一回事）");
                    }
                    if (packet.head_ignored > 0 && !head_noted_) {
                        head_noted_ = true;
                        host.Info("FoxyFace 上报了头姿键（Head*），单位与极化未确认，本版本不写入头姿通道");
                    }
                }
            } else {
                host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs, "丢弃一包：" + error);
            }
        }

        if (!staleness_.Check(host, endpoint_.LastPacketNs(), now_ns) || !parsed) {
            return false;
        }

        foxyface::WriteFrame(packet, table_, &slot);
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        endpoint_.Close();
        host.StatusStopped("FoxyFace 已停止");
    }

private:
    fgb::kit::UdpEndpoint endpoint_;
    fgb::kit::ShapeTable table_;
    fgb::kit::Staleness staleness_{kNoDataTimeoutNs, "FoxyFace"};
    std::uint16_t port_ = kDefaultPort;
    std::uint64_t warn_ns_ = 0;
    std::size_t reported_unknown_ = 0;
    std::size_t reported_invalid_ = 0;
    bool head_noted_ = false;
};

} // namespace

FGB_KIT_EXPORT(FoxyFaceSource, "com.fgbbridge.foxyface", "FoxyFace",
               FGB_CAP_EXPRESSION | FGB_CAP_EYES)
