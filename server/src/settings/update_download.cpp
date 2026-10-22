#include "settings/update_download.h"

#include "utils/common_utils.h"
#include "utils/network_proxy.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <regex>
#include <utility>
#include <vector>

namespace update_download
{
namespace
{
// 安装包上限。当前安装包不到 100 MB，留十倍余量；超过就当作 manifest 或重定向出了问题，
// 不去把用户磁盘写满。
constexpr std::uint64_t kMaxInstallerBytes = 1024ull * 1024 * 1024;
constexpr char kUpdatesDirectory[] = "updates";
constexpr char kStatusFile[] = "status.json";
constexpr char kPartSuffix[] = ".part";
constexpr char kMirrorPrefix[] = "https://dl.msime.app/gh/";

std::filesystem::path UpdatesRoot()
{
    const std::wstring data = CommonUtils::get_ime_data_path_w();
    if (data.empty())
        return {};
    return std::filesystem::path(data) / std::filesystem::path(kUpdatesDirectory);
}

// 版本号直接当目录名，必须先挡掉路径穿越：只允许字母数字和 . + -，且不能以点开头。
std::filesystem::path VersionDirectory(const std::string &version)
{
    static const std::regex safe(R"(^[0-9A-Za-z][0-9A-Za-z.+-]*$)");
    if (!std::regex_match(version, safe))
        return {};
    const std::filesystem::path root = UpdatesRoot();
    if (root.empty())
        return {};
    return root / std::filesystem::path(version);
}

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const std::wstring &wide)
{
    if (wide.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string text(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr, nullptr);
    return text;
}

std::uint64_t NowSeconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// status.json 是扁平的键值对，不引入 JSON 依赖：它跨两个进程共享，字段少且都在自己手里，
// 用最简单的格式就能避免「解析半截」之外的所有问题。写入走临时文件改名，读的一侧要么
// 看到旧内容，要么看到完整的新内容。
std::string EscapeField(const std::string &value)
{
    std::string escaped;
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string UnescapeField(const std::string &value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] != '\\' || i + 1 >= value.size())
        {
            result.push_back(value[i]);
            continue;
        }
        switch (value[++i])
        {
        case 'n':
            result.push_back('\n');
            break;
        case 'r':
            result.push_back('\r');
            break;
        default:
            result.push_back(value[i]);
            break;
        }
    }
    return result;
}

bool ParseUnsigned(const std::string &text, std::uint64_t &value)
{
    if (text.empty())
        return false;
    std::uint64_t parsed = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
            return false;
        parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    value = parsed;
    return true;
}

std::string HexDigest(const unsigned char raw[32])
{
    static const char kHex[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(64);
    for (size_t i = 0; i < 32; ++i)
    {
        digest.push_back(kHex[raw[i] >> 4]);
        digest.push_back(kHex[raw[i] & 0x0F]);
    }
    return digest;
}

// URL 拆成 host + path。只接受 HTTPS：安装包不能走明文通道。
bool CrackHttps(const std::string &url, std::wstring &host, std::wstring &path, INTERNET_PORT &port)
{
    if (url.rfind("https://", 0) != 0)
        return false;
    const std::wstring wide = Widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host_buffer[256]{};
    wchar_t path_buffer[4096]{};
    parts.lpszHostName = host_buffer;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host_buffer));
    parts.lpszUrlPath = path_buffer;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path_buffer));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    host.assign(host_buffer, parts.dwHostNameLength);
    path.assign(path_buffer, parts.dwUrlPathLength);
    port = parts.nPort;
    return !host.empty() && !path.empty();
}

enum class Outcome
{
    Ok,
    Network,
    TooLarge,
    Checksum,
    Io,
};

struct Transfer
{
    Outcome outcome = Outcome::Network;
    std::string detail;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
};

// 一次 GET，流式写盘并同时算 SHA-256。失败时把临时文件删掉，不留残片。
Transfer FetchToFile(const std::string &url, const std::filesystem::path &target,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress, std::string &digest)
{
    Transfer result;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!CrackHttps(url, host, path, port))
    {
        result.detail = "下载地址无效";
        return result;
    }

    // 与其它联网路径一致地走用户配置的代理，不在调用点各自 WinHttpOpen。
    HINTERNET session = NetworkProxy::OpenWinHttpSession(L"MetasequoiaImeServer/1.0");
    if (!session)
    {
        result.detail = "无法建立网络会话";
        return result;
    }
    // 连接和发送保持短超时，安装包动辄几十 MB，接收窗口放宽。
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    if (request)
    {
        // GitHub 的 release 资产会 302 到 CDN，镜像也会跳转，显式放开重定向。
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }

    const auto release = [&] {
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    };

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    const bool responded =
        request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    if (!responded || status != HTTP_STATUS_OK)
    {
        result.detail = "服务器没有返回安装包";
        release();
        return result;
    }

    DWORD length = 0;
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
    {
        result.total = length;
    }
    if (result.total > kMaxInstallerBytes)
    {
        result.outcome = Outcome::TooLarge;
        result.detail = "安装包超过允许的大小";
        release();
        return result;
    }

    std::error_code ec;
    std::filesystem::remove(target, ec);
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool hash_ready = file && BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    if (!hash_ready)
    {
        result.outcome = Outcome::Io;
        result.detail = "无法写入下载文件";
        release();
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        std::filesystem::remove(target, ec);
        return result;
    }

    std::vector<char> buffer(64 * 1024);
    result.outcome = Outcome::Ok;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
        {
            if (available == 0 && GetLastError() == ERROR_SUCCESS)
                break;
            result.outcome = Outcome::Network;
            result.detail = "下载中断";
            break;
        }
        const DWORD want = (std::min)(available, static_cast<DWORD>(buffer.size()));
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), want, &read) || read == 0)
        {
            result.outcome = Outcome::Network;
            result.detail = "下载中断";
            break;
        }
        if (result.received + read > kMaxInstallerBytes)
        {
            result.outcome = Outcome::TooLarge;
            result.detail = "安装包超过允许的大小";
            break;
        }
        file.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!file)
        {
            result.outcome = Outcome::Io;
            result.detail = "写入下载文件失败";
            break;
        }
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) != 0)
        {
            result.outcome = Outcome::Checksum;
            result.detail = "无法计算校验值";
            break;
        }
        result.received += read;
        if (progress)
            progress(result.received, result.total);
    }
    file.close();
    release();

    unsigned char raw[32]{};
    if (result.outcome == Outcome::Ok && BCryptFinishHash(hash, raw, sizeof(raw), 0) != 0)
    {
        result.outcome = Outcome::Checksum;
        result.detail = "无法计算校验值";
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (result.outcome == Outcome::Ok && result.total != 0 && result.received != result.total)
    {
        // 服务端声明了长度却没给全，不能当完整文件用。
        result.outcome = Outcome::Network;
        result.detail = "下载不完整";
    }
    if (result.outcome == Outcome::Ok)
    {
        digest = HexDigest(raw);
    }
    else
    {
        std::filesystem::remove(target, ec);
        result.received = 0;
    }
    return result;
}
} // namespace

bool IsSha256Hex(const std::string &text)
{
    if (text.size() != 64)
        return false;
    for (const unsigned char ch : text)
        if (!std::isdigit(ch) && !(ch >= 'a' && ch <= 'f'))
            return false;
    return true;
}

bool IsInstallerName(const std::string &name)
{
    static const std::regex pattern(R"(^MetasequoiaIME_Setup_v[\w.-]+\.exe$)", std::regex::icase);
    return name.size() <= 120 && std::regex_match(name, pattern);
}

std::string MirrorUrlFor(const std::string &github_url)
{
    static const std::regex tag(R"(^https://github\.com/metasequoiaime/[\w.-]+/releases/download/[\w.+-]+/[\w.-]+$)");
    if (!std::regex_match(github_url, tag))
        return {};
    return std::string(kMirrorPrefix) + github_url;
}

std::filesystem::path StatusPath()
{
    const std::filesystem::path root = UpdatesRoot();
    return root.empty() ? std::filesystem::path() : root / std::filesystem::path(kStatusFile);
}

Progress ReadStatus()
{
    Progress progress;
    const std::filesystem::path path = StatusPath();
    if (path.empty())
        return progress;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return progress;
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        fields[line.substr(0, equals)] = UnescapeField(line.substr(equals + 1));
    }
    std::uint64_t state = 0;
    if (!ParseUnsigned(fields["state"], state) || state > static_cast<std::uint64_t>(State::TooLarge))
        return progress;
    progress.state = static_cast<State>(state);
    progress.version = fields["version"];
    progress.installer = fields["installer"];
    progress.error = fields["error"];
    (void)ParseUnsigned(fields["received"], progress.received);
    (void)ParseUnsigned(fields["total"], progress.total);
    (void)ParseUnsigned(fields["updated_at"], progress.updated_at);
    return progress;
}

void WriteStatus(const Progress &progress)
{
    const std::filesystem::path path = StatusPath();
    if (path.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::string text;
    text += "state=" + std::to_string(static_cast<int>(progress.state)) + "\n";
    text += "version=" + EscapeField(progress.version) + "\n";
    text += "installer=" + EscapeField(progress.installer) + "\n";
    text += "error=" + EscapeField(progress.error) + "\n";
    text += "received=" + std::to_string(progress.received) + "\n";
    text += "total=" + std::to_string(progress.total) + "\n";
    text += "updated_at=" + std::to_string(progress.updated_at != 0 ? progress.updated_at : NowSeconds()) + "\n";
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return;
        file << text;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(temporary, ec);
}

std::filesystem::path InstallerPathFor(const std::string &version, const std::string &installer)
{
    if (!IsInstallerName(installer))
        return {};
    const std::filesystem::path directory = VersionDirectory(version);
    if (directory.empty())
        return {};
    const std::filesystem::path path = directory / std::filesystem::path(installer);
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path();
}

void PruneOldVersions(const std::string &keep_version)
{
    const std::filesystem::path root = UpdatesRoot();
    if (root.empty())
        return;
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> directories;
    for (const auto &entry : std::filesystem::directory_iterator(root, ec))
    {
        if (entry.is_directory(ec))
            directories.push_back(entry);
    }
    if (directories.size() <= kRetainVersions)
        return;
    // 目录名就是版本号，三段数字按字典序恰好等于语义序；同主版本的预发布后缀也排得对。
    std::sort(directories.begin(), directories.end(),
              [](const std::filesystem::directory_entry &left, const std::filesystem::directory_entry &right) {
                  return left.path().filename().wstring() < right.path().filename().wstring();
              });
    while (directories.size() > kRetainVersions)
    {
        const auto &oldest = directories.front();
        if (oldest.path().filename() == std::filesystem::path(keep_version))
        {
            // 要保留的那一版恰好最旧：留下它，改删下一个。
            if (directories.size() < 2)
                break;
            std::rotate(directories.begin(), directories.begin() + 1, directories.end());
            continue;
        }
        std::filesystem::remove_all(oldest.path(), ec);
        directories.erase(directories.begin());
    }
}

Result Download(const std::string &version, const std::string &installer, const std::string &github_url,
                const std::string &mirror_url, const std::string &expected_sha256,
                const std::function<void(std::uint64_t received, std::uint64_t total)> &progress)
{
    Result result;
    if (!IsSha256Hex(expected_sha256))
    {
        // 没有摘要就不自动下载：manifest 没给校验值等于放弃完整性判断。
        result.error = "更新清单没有提供有效的 SHA256";
        return result;
    }
    if (!IsInstallerName(installer))
    {
        result.error = "更新清单里的安装包名无效";
        return result;
    }
    const std::filesystem::path directory = VersionDirectory(version);
    if (directory.empty())
    {
        result.error = "无法确定下载目录";
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        result.error = "无法创建下载目录";
        return result;
    }

    const std::filesystem::path final_path = directory / std::filesystem::path(installer);
    const std::filesystem::path part_path = [&] {
        std::filesystem::path path = final_path;
        path += std::filesystem::path(kPartSuffix);
        return path;
    }();

    // 先试 GitHub，失败再试镜像。两边都失败时把最后一次失败原因带出去。
    std::vector<std::string> urls;
    if (!github_url.empty())
        urls.push_back(github_url);
    if (!mirror_url.empty())
        urls.push_back(mirror_url);

    Outcome last = Outcome::Network;
    std::string last_detail = "没有可用的下载地址";
    for (const std::string &url : urls)
    {
        std::string digest;
        const Transfer transfer = FetchToFile(url, part_path, progress, digest);
        result.received = transfer.received;
        result.total = transfer.total;
        if (transfer.outcome == Outcome::TooLarge)
        {
            result.state = State::TooLarge;
            result.error = transfer.detail;
            return result;
        }
        if (transfer.outcome != Outcome::Ok)
        {
            last = transfer.outcome;
            last_detail = transfer.detail;
            continue;
        }
        if (digest != expected_sha256)
        {
            std::filesystem::remove(part_path, ec);
            result.state = State::Failed;
            result.error = "安装包校验失败";
            result.received = 0;
            return result;
        }
        std::filesystem::remove(final_path, ec);
        if (!MoveFileExW(part_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(part_path, ec);
            result.error = "无法保存安装包";
            return result;
        }
        result.state = State::Ready;
        return result;
    }

    result.state = last == Outcome::Io ? State::Failed : State::Failed;
    result.error = last_detail;
    result.received = 0;
    return result;
}
} // namespace update_download
