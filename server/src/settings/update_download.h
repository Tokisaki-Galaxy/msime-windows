#pragma once

// 自动下载安装包。只在用户打开 appearance.auto_download_update 之后工作，且只在发布通道
// 上生效：正式版跟上正式版，nightly 跟上 nightly，dev 构建永远不参与（见 update_channel.h）。
//
// 这里是下载器本身，不含调度。状态落在磁盘上，设置进程与服务端两侧读的是同一份事实，
// 任何时候关掉设置窗口都不影响已经开始的下载，反过来也一样。
//
// 落盘布局（数据目录下，与 models/ 同级）：
//   updates/status.json          进度与结果，随时可覆盖，不参与任何安全判断
//   updates/<版本>/<包名>        校验通过的安装包，是唯一的「可以安装」依据
//   updates/<版本>/<包名>.part   下载中的临时文件，同一卷上原子改名
//
// 校验只有摘要一层：manifest 里的 installerSha256 必须与下载到的字节完全一致。
// 不一致就删掉临时文件，绝不留可执行的残片。这个摘要同样来自 manifest，所以它证明的是
// 传输完整，不证明来源；来源可信度由 update.json 走 HTTPS 从官网取得这一条承担。
// 签名校验是另一件事，不在本模块范围内。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace update_download
{
// 下载状态。数值稳定，会被写进 status.json 并由设置页原样读走。
enum class State
{
    Idle = 0,
    Downloading = 1,
    Ready = 2,
    Failed = 3,
    TooLarge = 4,
};

struct Progress
{
    State state = State::Idle;
    std::string version;        // 目标版本号；Ready 时就是以它命名的安装包
    std::string installer;      // 安装包文件名，Ready 时非空
    std::string error;          // 中文原因，Failed / TooLarge 时非空
    std::uint64_t received = 0; // 已收字节
    std::uint64_t total = 0;    // 服务端声明的总长；0 表示没声明
    std::uint64_t updated_at = 0;
};

// 状态文件路径。数据目录不可用时返回空路径。
std::filesystem::path StatusPath();

// reads status.json；文件不存在或内容损坏时返回 Idle，绝不因此报错。
Progress ReadStatus();

// 写 status.json。走临时文件加改名，读的一侧不会看到半截 JSON。
void WriteStatus(const Progress &progress);

// 校验通过的安装包路径；没有可用的包时返回空路径。只信文件系统，不读 status.json，
// 用户手工删掉包之后这里立刻返回空。
std::filesystem::path InstallerPathFor(const std::string &version, const std::string &installer);

// 保留的版本目录数量上限，超出时按名字排序删最旧的。
inline constexpr size_t kRetainVersions = 2;

// 删掉超出上限的旧版本目录，`keep_version` 永远保留。
void PruneOldVersions(const std::string &keep_version);

// 安装包文件名：MetasequoiaIME_Setup_v<版本>.exe，字母数字点横线，大小写不敏感。
bool IsInstallerName(const std::string &name);

// manifest 里 installerSha256 的合法性：64 个小写十六进制字符。
bool IsSha256Hex(const std::string &text);

// 把 GitHub 的资产地址映射到国内镜像前缀；无法映射时返回空串。
std::string MirrorUrlFor(const std::string &github_url);

// 一次下载的结局。total 为 0 表示服务端没声明长度。
struct Result
{
    State state = State::Failed;
    std::string error;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
};

// 阻塞式下载并校验，返回的文件只在 state == Ready 时存在于 InstallerPathFor 上。
//
// github_url 优先，失败时回退到 mirror_url（为空则跳过）。两处都下不到就返回 Failed。
// expected_sha256 必须是 IsSha256Hex 认可的 64 个十六进制字符，否则直接失败：
// 没有摘要的 manifest 不能用来自动下载。
//
// progress 每收一块回调一次，参数是当前地址的（已收，总长）。
Result Download(const std::string &version, const std::string &installer, const std::string &github_url,
                const std::string &mirror_url, const std::string &expected_sha256,
                const std::function<void(std::uint64_t received, std::uint64_t total)> &progress);
} // namespace update_download
