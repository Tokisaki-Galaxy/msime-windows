#include "settings/update_check.h"

#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "msime_version.h"
#include "settings/settings_launcher.h"
#include "settings/update_channel.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

namespace update_check
{
namespace
{
constexpr char kManifestUrl[] = "https://msime.app/update.json";

std::mutex &Mutex()
{
    static std::mutex mutex;
    return mutex;
}

// 进行中的目标版本，防止轮询间隔内重复起下载。
std::string &ActiveVersion()
{
    static std::string version;
    return version;
}

// GET 一个文本地址。成功返回 true。与其它联网路径一致地走用户配置的代理。
bool HttpGetText(const std::string &url, std::string &body)
{
    const auto widen = [](const std::string &text) {
        if (text.empty())
            return std::wstring();
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>((std::max)(length, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    };

    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts))
        return false;

    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (!session)
        return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request =
        connection
            ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
            : nullptr;
    if (request)
    {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    bool ok = request &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr) &&
              WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) &&
              status == HTTP_STATUS_OK;
    if (ok)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0 && body.size() < 1024 * 1024)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
                break;
            body.append(chunk.data(), read);
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

// 把四段载荷拼成契约里的一行。所有字段都是本机产生的短文本，不含制表符本身。
std::wstring InstallerPayload(const std::string &version, const std::filesystem::path &path,
                              const std::string &installer, const std::string &release_url)
{
    const auto widen = [](const std::string &text) {
        if (text.empty())
            return std::wstring();
        const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>((std::max)(length, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    };
    std::wstring payload = widen(version);
    payload += L'\t';
    payload += path.wstring();
    payload += L'\t';
    payload += widen(installer);
    payload += L'\t';
    payload += widen(release_url);
    return payload;
}

// 通知所有已连接的 TSF 客户端。用 worker 管道的原因在 contracts 的 opcode 注释里：
// 那里本来就是广播推荐给所有 TIP 的通道，不受焦点客户端限制。
void BroadcastInstallerReady(const std::string &version, const std::filesystem::path &path,
                             const std::string &installer, const std::string &release_url)
{
    const std::wstring payload = InstallerPayload(version, path, installer, release_url);
    BroadcastToTsfWorkerThreadViaNamedpipe(Global::DataFromServerMsgTypeToTsfWorkerThread::UpdateInstallerReady,
                                           payload);
}

struct Candidate
{
    std::string version;
    std::string text; // 规范化后的版本号，用来命名目录
    std::string release_url;
    std::string installer;
    std::string installer_url; // 清单可选字段，指向同一个 tag 下的同一份资产
    std::string sha256;
};

// 发布页地址里的 tag；不是本仓库的 releases/tag/ 形式时返回空串。
std::string TagOf(const std::string &release_url)
{
    static const std::regex tag(R"(^https://github\.com/metasequoiaime/[\w.-]+/releases/tag/([\w.+-]+)$)");
    std::smatch match;
    if (!std::regex_match(release_url, match, tag))
        return {};
    return match[1].str();
}

// 解析 manifest 并与当前版本比对。任何一处不合法都判为「没有更新」，不让一条坏数据
// 变成下载请求。
bool ResolveCandidate(const std::string &body, update_channel::Version &current, Candidate &candidate)
{
    update_channel::Parse(CurrentVersion(), current);
    if (!current.valid)
        return false;

    const nlohmann::json manifest = nlohmann::json::parse(body, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("version") || !manifest["version"].is_string() ||
        !manifest.contains("releaseUrl") || !manifest["releaseUrl"].is_string())
    {
        return false;
    }
    const std::string raw_version = manifest["version"].get<std::string>();
    const std::string release_url = manifest["releaseUrl"].get<std::string>();

    update_channel::Version latest;
    if (!update_channel::Parse(raw_version, latest) || !latest.valid)
        return false;
    // 通道必须一致。这条同时挡掉了「正式版被 nightly 清单顶掉」和「nightly 被正式版顶掉」。
    if (latest.kind != current.kind)
        return false;
    if (update_channel::Compare(latest, current) <= 0)
        return false;

    // 发布页地址必须指向本仓库，且 tag 与版本号对得上，避免清单里塞一个任意地址。
    const std::string tag = update_channel::TagFromReleaseUrl(release_url);
    if (tag.empty())
        return false;
    update_channel::Version from_tag;
    if (!update_channel::Parse(tag, from_tag) || update_channel::Compare(from_tag, latest) != 0)
        return false;

    candidate.version = raw_version;
    candidate.text = latest.text;
    candidate.release_url = release_url;
    candidate.installer = manifest.value("installerName", std::string());
    candidate.installer_url = manifest.value("installerUrl", std::string());
    candidate.sha256 = manifest.value("installerSha256", std::string());
    // 没有可用的安装包或摘要时仍然是有更新的，只是不能自动下载，交给设置页提示。
    return true;
}

std::string InstallerUrlFor(const Candidate &candidate)
{
    // 优先用清单自带的 installerUrl。官网现在就是这么给的，按它取能避免把两份信息
    // 各自推演一遍；必须指向同一个 tag 下的同一份资产，否则说明清单自相矛盾，宁可不下。
    static const std::regex asset(
        R"(^(https://github\.com/metasequoiaime/[\w.-]+)/releases/download/([\w.+-]+)/([\w.-]+)$)");
    std::smatch asset_match;
    if (std::regex_match(candidate.installer_url, asset_match, asset))
    {
        if (asset_match[3].str() == candidate.installer && asset_match[2].str() == TagOf(candidate.release_url))
            return candidate.installer_url;
        return {};
    }

    // 清单没给 installerUrl 时，从发布页地址推：releases/tag/<tag> → releases/download/<tag>/<name>。
    static const std::regex tag(R"(^(https://github\.com/metasequoiaime/[\w.-]+)/releases/tag/([\w.+-]+)$)");
    std::smatch match;
    if (!std::regex_match(candidate.release_url, match, tag))
        return {};
    return match[1].str() + "/releases/download/" + match[2].str() + "/" + candidate.installer;
}

// 落盘并广播。进度写入 status.json，设置页靠它显示。
void RunDownload(const Candidate &candidate)
{
    const std::string github = InstallerUrlFor(candidate);
    const std::string mirror = update_download::MirrorUrlFor(github);

    update_download::Progress progress;
    progress.state = update_download::State::Downloading;
    progress.version = candidate.text;
    progress.installer = candidate.installer;
    update_download::WriteStatus(progress);

    const auto publish = [&candidate](std::uint64_t received, std::uint64_t total) {
        update_download::Progress snapshot;
        snapshot.state = update_download::State::Downloading;
        snapshot.version = candidate.text;
        snapshot.installer = candidate.installer;
        snapshot.received = received;
        snapshot.total = total;
        update_download::WriteStatus(snapshot);
    };

    const update_download::Result result =
        update_download::Download(candidate.text, candidate.installer, github, mirror, candidate.sha256, publish);

    update_download::Progress done;
    done.state = result.state;
    done.version = candidate.text;
    done.installer = result.state == update_download::State::Ready ? candidate.installer : std::string();
    done.error = result.error;
    done.received = result.received;
    done.total = result.total;
    update_download::WriteStatus(done);

    if (result.state != update_download::State::Ready)
        return;
    update_download::PruneOldVersions(candidate.text);

    // 路径由 TSF 侧直接交给 ShellExecute，保持宽字符，不经 UTF-8 往返。
    const std::filesystem::path path = update_download::InstallerPathFor(candidate.text, candidate.installer);
    if (path.empty())
        return;
    BroadcastInstallerReady(candidate.text, path, candidate.installer, candidate.release_url);
}

} // namespace

std::string CurrentVersion()
{
    return MSIME_PRODUCT_VERSION;
}

bool HasPendingInstaller(const std::string &current_version, update_download::Progress &progress)
{
    // 用户关掉自动下载之后就不再有任何提示，哪怕上次已经下好了包：这个开关控制的是
    // 「要不要让输入法自己碰更新」，不只是「要不要联网」。
    if (!GetConfiguredAutoDownloadUpdate())
        return false;
    update_channel::Version current;
    if (!update_channel::Parse(current_version, current) || !update_channel::SupportsAutoUpdate(current))
        return false;
    progress = update_download::ReadStatus();
    if (progress.state != update_download::State::Ready)
        return false;
    update_channel::Version ready;
    if (!update_channel::Parse(progress.version, ready) || ready.kind != current.kind)
        return false;
    return update_channel::Compare(ready, current) > 0;
}

bool LaunchPendingInstaller(const std::wstring &requested_installer)
{
    // 路径只从磁盘上的状态推出来，不采信消息里带的内容：消息只用来核对是不是同一个文件。
    update_download::Progress progress;
    if (!HasPendingInstaller(CurrentVersion(), progress))
        return false;
    const std::filesystem::path path = update_download::InstallerPathFor(progress.version, progress.installer);
    if (path.empty() || path.filename().wstring() != requested_installer)
        return false;
    // 安装器会替换本进程和几个面板 exe，先让它们退出，避免 Inno 列出「需要关闭的程序」
    // 逼用户重启。设置窗口自己由宿主在拉起来之后关掉，这里只管同目录的兄弟进程。
    CloseEmojiPanelApplication();
    CloseKeyboardPanelApplication();
    CloseHandwritingPanelApplication();
    // 安装包需要提权，交给 ShellExecute 走 UAC；不放参数，让用户在 Inno Setup 里自己确认。
    const HINSTANCE result =
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, path.parent_path().c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void CheckOnce()
{
    if (!GetConfiguredAutoDownloadUpdate())
        return;
    update_channel::Version current;
    if (!update_channel::Parse(CurrentVersion(), current) || !update_channel::SupportsAutoUpdate(current))
        return;

    // 缓存穿透：加一个时间戳，免得 CDN 一直返回旧清单。
    const long long stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::string body;
    if (!HttpGetText(std::string(kManifestUrl) + "?t=" + std::to_string(stamp), body))
        return;

    Candidate candidate;
    update_channel::Version latest;
    if (!ResolveCandidate(body, current, candidate))
        return;
    if (!update_download::IsInstallerName(candidate.installer) || !update_download::IsSha256Hex(candidate.sha256))
        return;
    if (candidate.text == current.text)
        return;
    const std::filesystem::path existing = update_download::InstallerPathFor(candidate.text, candidate.installer);
    if (!existing.empty())
    {
        // 已经下过并校验过了，只差提示。
        BroadcastInstallerReady(candidate.text, existing, candidate.installer, candidate.release_url);
        return;
    }

    {
        std::lock_guard lock(Mutex());
        if (!ActiveVersion().empty())
            return;
        ActiveVersion() = candidate.text;
    }
    RunDownload(candidate);
    std::lock_guard lock(Mutex());
    ActiveVersion().clear();
}

namespace
{
std::atomic<bool> g_started{false};
std::thread g_thread;

void PollLoop()
{
    // 启动后先让一让，避开开机时的网络和其它启动任务。
    std::this_thread::sleep_for(std::chrono::minutes(kStartupDelayMinutes));
    for (;;)
    {
        CheckOnce();
        std::this_thread::sleep_for(std::chrono::minutes(kPollIntervalMinutes));
    }
}
} // namespace

void StartPolling()
{
    if (g_started.exchange(true))
        return;
    // 轮询线程跟着进程活着，退出时直接 detach：这条路只跑进程启动后一次，而且
    // 停下来等它收尾只会把退出拖成几秒。没下完的 .part 下次会被覆盖，不会留下
    // 可以误装的残片。
    g_thread = std::thread(PollLoop);
    g_thread.detach();
}
} // namespace update_check
