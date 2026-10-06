/**
 * 文件功能：命令行输入接口
 *
 * 职责说明：声明命令参数切分和输入线程的读取循环。
 * 调用关系：main 启动 cli_loop；业务线程在 commands.cpp 中解释收到的整行命令。
 * 阅读提示：输入读取和业务执行分属不同线程；带空格的工件编号可用双引号括起来。
 */
#pragma once
#include "inspection/events.hpp"
#include "inspection/instrument.hpp"
#include <functional>
#include <string>
namespace inspection {
// 输入模块仅切分参数并投递事件，业务合法性在 InspectionWorker 判断。
// 模块一：参数切分。支持双引号字符串，例如 start "编号 A" demo1 得到三个参数。
std::vector<std::string> cli_words(const std::string& line);
// 模块二：输入线程。input_fd 为输入描述符，stop_fd 为退出唤醒管道，
// sink 投递事件，stopping 提供共享停止状态；此函数不直接修改检测任务。
void cli_loop(int input_fd, int stop_fd, EventSink, const std::function<bool()>& stopping);
} // namespace inspection
