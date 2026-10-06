/**
 * 文件功能：项目核心功能的五组单元测试
 *
 * 职责说明：验证质量边界、工位协议、仪器解析、队列时钟、数据库事务与云接口行为。
 * 调用关系：CTest 运行 inspection_tests → main 依次调用 test_* → CHECK 失败则抛异常。
 * 阅读提示：这些测试无需真实 BLE/串口；异常注入用于验证数据库回滚，不能代替硬件验收。
 */
#include "inspection/cli.hpp"
#include "inspection/cloud.hpp"
#include "inspection/protocol.hpp"
#include "inspection/repository.hpp"
#include "inspection/support.hpp"
#include "inspection/types.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thickness/protocol.hpp>
// 模块一：轻量断言工具。#x 把表达式转字符串，失败时抛异常并显示具体表达式。
// do { ... } while (0) 使多语句宏在 if 等上下文里表现为一条语句。
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("检查失败：") + #x);                              \
    } while (0)
using namespace inspection;
namespace {
// 验证“应该拒绝”的操作确实抛了异常；正常返回会触发 CHECK 失败。
template <class F> void rejects(F f) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception&) {
        failed = true;
    }
    CHECK(failed);
}
// 模块二：质量边界。上下界可通过，超过/低于一单位失败，材料或点数错误也失败。
void test_rule() {
    Rule r;
    r.required_points = 1;
    r.substrate = "FE";
    Point p;
    p.value.substrate = "FE";
    p.value.thickness_milli_um = r.min_milli_um;
    CHECK(passes(r, {p}));
    p.value.thickness_milli_um = r.max_milli_um;
    CHECK(passes(r, {p}));
    ++p.value.thickness_milli_um;
    CHECK(!passes(r, {p}));
    p.value.thickness_milli_um = r.min_milli_um - 1;
    CHECK(!passes(r, {p}));
    p.value.thickness_milli_um = 150000;
    p.value.substrate = "NFE";
    CHECK(!passes(r, {p}));
    CHECK(!passes(r, {}));
}
// 模块三：工位协议。用显著的十六进制值检查高低字顺序，再验证 ACK 和状态一致性。
void test_protocol() {
    Command c;
    c.epoch = 0x12345678;
    c.token = 0x9abcdef0;
    c.sequence = 0x23456789;
    c.opcode = Opcode::ApplyResult;
    c.argument = 1;
    auto words = encode(c);
    CHECK(words[0] == 0x1234 && words[1] == 0x5678 && words[2] == 0x9abc && words[3] == 0xdef0 &&
          words[6] == 3 && words[8] == 1);
    // 构造一个完成且 PASS 的工位快照，匹配命令 epoch/token/序号，检查成功路径。
    std::array<std::uint16_t, 22> raw{};
    raw[0] = 4;
    raw[1] = 1;
    raw[2] = 1;
    raw[8] = words[2];
    raw[9] = words[3];
    raw[10] = words[4];
    raw[11] = words[5];
    raw[12] = 1;
    raw[19] = 0x0200;
    raw[20] = words[0];
    raw[21] = words[1];
    auto snapshot = decode(raw);
    CHECK(command_satisfied(c, snapshot));
    // 旧周期 ACK 即使其他字段正确也应失败；MCU 处于故障时也不能确认结果已应用。
    snapshot.ack_epoch++;
    CHECK(!command_satisfied(c, snapshot));
    snapshot.ack_epoch = c.epoch;
    snapshot.state = McuState::Fault;
    CHECK(!command_satisfied(c, snapshot));
    // MCU 仅有 0～5 状态，未知编码必须拒绝。
    raw[0] = 6;
    rejects([&] { decode(raw); });
}
// 模块四：仪器解析。测试分包、多帧、前导噪声、CRC 损坏、断线 reset 和负厚度。
void test_parser() {
    // 此完整帧来自上游 README 的已验证样例；本测试不是重新进行仪器实测。
    std::vector<std::uint8_t> frame = {0xbb, 2,    0x18, 0,    0x64, 0x44, 0x16, 0,   0,
                                       0x44, 0x65, 0x66, 0x61, 0x75, 0x6c, 0x74, 0,   0,
                                       0,    0,    0,    0,    0,    0,    0,    0x85};
    thickness::FrameParser parser;
    // 先送 10 字节半帧，不应立即生成测量；补齐剩余字节后恰好得到一个测量。
    auto first = parser.feed(frame.data(), 10);
    CHECK(first.measurements.empty());
    auto second = parser.feed(frame.data() + 10, frame.size() - 10);
    CHECK(second.measurements.size() == 1);
    CHECK(second.measurements[0].milli_um == 1459300);
    CHECK(second.measurements[0].material == thickness::MaterialType::nfe);
    // 噪声后连续拼两帧，应一次得到两个结果，不按相同厚度去重。
    std::vector<std::uint8_t> joined = {1, 2, 3};
    joined.insert(joined.end(), frame.begin(), frame.end());
    joined.insert(joined.end(), frame.begin(), frame.end());
    CHECK(parser.feed(joined).measurements.size() == 2);
    // 翻转 CRC 一位注入传输错误，确认错误计数增加。
    auto broken = frame;
    broken.back() ^= 1;
    CHECK(parser.feed(broken).crc_errors > 0);
    parser.reset();
    parser.feed(frame.data(), 10);
    parser.reset();
    CHECK(parser.feed(frame.data() + 10, 16).measurements.empty());
    parser.reset();
    // 设置协议符号位并重算 CRC，确保解析器保留负值，而不是按普通补码解释。
    frame[7] |= 0x80;
    frame.back() = thickness::crc8(frame.data() + 1, 24);
    CHECK(parser.feed(frame).measurements[0].milli_um == -1459300);
}
// 模块五：队列和时钟。容量一的队列可直观验证满载、溢出标志与关闭行为。
void test_queue_clock() {
    BoundedQueue<int> q(1);
    CHECK(q.push(1));
    CHECK(!q.push(2));
    CHECK(q.take_overflow());
    int result = 0;
    CHECK(q.pop(result, std::chrono::milliseconds(0)) && result == 1);
    q.close();
    CHECK(!q.push(3));
    CHECK(!q.pop(result, std::chrono::milliseconds(0)));
    ManualClock clock;
    clock.monotonic = 100;
    clock.wall = 500;
    CHECK(clock.mono_ms() == 100);
    // 模拟日历时钟跳变，单调时钟不应受影响；同时验证带空格工件编号的参数切分。
    clock.wall = -100;
    CHECK(clock.mono_ms() == 100);
    CHECK(cli_words("start \"编号 A\" demo5").size() == 3);
}
// 模块六：持久化与云接口。使用独立临时目录，离开测试后删除，避免污染实际检测数据库。
void test_storage() {
    const auto dir = std::filesystem::temp_directory_path() / uuid();
    std::filesystem::create_directories(dir);
    const auto path = (dir / "inspection.db").string();
    {
        InspectionRepository r(path);
        // 检查身份持久化计数从非零开始且逐次递增。
        auto epoch = r.allocate("host_epoch");
        CHECK(epoch == 1);
        CHECK(r.allocate("host_epoch") == 2);
        // 准备完整单点检测，按实际业务顺序 create → mark_started → save_point → complete。
        Task t;
        t.id = uuid();
        t.workpiece = "A";
        t.rule.product_id = "demo";
        t.rule.version = "1";
        t.rule.substrate = "FE";
        t.rule.required_points = 1;
        t.token = r.allocate("session_token");
        t.started_ms = 100;
        r.create(t, "UNVERIFIED", "run");
        r.mark_started(t.id);
        Point point;
        point.index = 1;
        point.value.candidate_id = uuid();
        point.value.substrate = "FE";
        point.value.thickness_milli_um = 150000;
        point.value.generation = 1;
        point.confirmed_ms = 200;
        r.save_point(t.id, point);
        // 重复确认同一索引必须被拒绝，不能覆盖已经保存的测量。
        rejects([&] { r.save_point(t.id, point); });
        t.points.push_back(point);
        t.result = "PASS";
        // 在最终 COMMIT 前抛异常，验证完成状态和 Outbox 插入同时回滚。
        r.checkpoint = [](const std::string& stage) {
            if (stage == "before_final_commit")
                throw std::runtime_error("故障注入");
        };
        rejects([&] { r.complete(t, "s", 300); });
        // 回滚后无待传事件、任务仍在 INSPECTING；随后清掉注入钩子再正常完成。
        CHECK(r.pending_count() == 0);
        CHECK(r.query(t.id)[0]["state"] == "INSPECTING");
        // 正常提交后恰好一个 Outbox 事件，尝试次数为零，载荷结果为 PASS，禁止再次完成。
        r.checkpoint = {};
        r.complete(t, "s", 300);
        CHECK(r.pending_count() == 1);
        CHECK(r.outbox()[0]["attempt_count"] == 0);
        CHECK(r.outbox()[0]["payload"]["result"] == "PASS");
        rejects([&] { r.complete(t, "s", 300); });
        Command c;
        c.epoch = 2;
        c.sequence = 1;
        c.opcode = Opcode::ApplyResult;
        c.token = t.token;
        r.record_command(t.id, c);
        // 再建未完成任务并模拟服务重启恢复：中止未完成任务、保留确认点和已完成质量结果。
        Task incomplete = t;
        incomplete.id = uuid();
        incomplete.token = r.allocate("session_token");
        r.create(incomplete, "UNVERIFIED", "run");
        r.mark_started(incomplete.id);
        r.save_point(incomplete.id, point);
        r.recover(400);
        CHECK(r.query(incomplete.id)[0]["state"] == "ABORTED");
        CHECK(r.query(incomplete.id)[0]["measurements"].size() == 1);
        CHECK(r.query(t.id)[0]["result"] == "PASS");
        CHECK(r.query(t.id)[0]["actuation"][0]["status"] == "UNKNOWN");
        // 未配置云必须返回 NotConfigured，不能调用成功回调，也不能增加数据库尝试次数。
        DisabledCloudPublisher cloud;
        bool called = false;
        CHECK(cloud.publish(r.outbox()[0]["payload"], "request", [&](CloudReply) {
            called = true;
        }) == PublishSubmission::NotConfigured);
        CHECK(!called && r.outbox()[0]["attempt_count"] == 0);
    }
    {
        // 只读连接真实写探针必须失败，验证仓库不会仅凭文件可读而宣称可写。
        InspectionRepository readonly(path, 100, true);
        rejects([&] { readonly.writable_probe(); });
    }
    std::filesystem::remove_all(dir);
}
} // namespace
// 模块七：测试入口。五组依次执行，任何异常打印原因并返回 1，供 CTest 判定失败。
int main() {
    try {
        test_rule();
        test_protocol();
        test_parser();
        test_queue_clock();
        test_storage();
        std::cout << "5 组单元测试通过：规则、协议、仪器解析、队列时钟、持久化与云接口\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
