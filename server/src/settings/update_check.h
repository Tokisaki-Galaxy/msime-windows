#pragma once

// 检查更新与自动下载安装包。只在 Server 进程里跑：Server 常驻，用户关不关设置窗口都不影响
// 它，下载也就不会因为关窗口中断。设置页只读状态，自己不再下载。
//
// 通道隔离是硬要求：正式版只跟正式版，nightly 只跟 nightly，dev 构建不参与自动下载。
// 判断依据是编译进去的版本号本身（见 update_channel.h），不需要额外的配置项。
//
// 一次探测的完整流程：
//   1. 读 config 里的 appearance.auto_download_update，关着就什么都不做；
//   2. GET https://msime.app/update.json，取 version / releaseUrl / installerName /
//      installerSha256，版本必须是同一个通道且比当前新；
//   3. 已有校验通过的包就跳过；否则下载，边下边把进度写进 status.json；
//   4. 摘要对上才落盘成最终名字，再通过 UpdateInstallerReady 通知所有已连接的 TSF 客户端。
//
// 失败一律静默收敛到下一次轮询：更新检查出问题是用户处理不了的，弹窗只会打扰。

#include "settings/update_download.h"

#include <string>

namespace update_check
{
// 探测间隔。下载体量几十 MB，一天一次足够；启动后先等一段时间，避开开机时的网络和其它
// 启动任务。
inline constexpr unsigned kPollIntervalMinutes = 6 * 60;
inline constexpr unsigned kStartupDelayMinutes = 2;

// 当前进程改用链接时刻的版本号，设置页和服务端两侧对同一台机器必须给出同一结论。
std::string CurrentVersion();

// 状态文件里的包是否比 current_version 新且同通道。设置页用它决定是否给出提示。
bool HasPendingInstaller(const std::string &current_version, update_download::Progress &progress);

// 用户在弹窗里同意安装时调用。只认磁盘上校验过的那一份，requested_installer 只用来
// 核对是不是同一个文件，不作为路径来源。返回是否确实把安装程序拉起来。
bool LaunchPendingInstaller(const std::wstring &requested_installer);

// 后台轮询线程。进程启动后创建一次，之后跟着进程活着；它自己不持有任何需要收尾的资源，
// 所以没有对应的停机接口。
void StartPolling();
} // namespace update_check
