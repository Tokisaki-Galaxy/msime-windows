#include "settings/update_channel.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace update_channel
{
namespace
{
// 主版本只接受数字段；预发布标签是第一个 '-' 之后的全部内容，'+' 之后视为构建元数据和
// 版本比较无关，直接丢掉。允许前导的 v/V，因为标签和安装包名都这么写。
constexpr char kVersionPattern[] = R"(^\s*[vV]?(\d+(?:\.\d+)*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?\s*$)";
constexpr char kTagPattern[] = R"(^https://github\.com/metasequoiaime/[\w.-]+/releases/tag/([\w.+-]+)$)";

bool SplitPrerelease(const std::string &prerelease, std::vector<std::string> &identifiers)
{
    identifiers.clear();
    size_t start = 0;
    while (start <= prerelease.size())
    {
        const size_t dot = prerelease.find('.', start);
        const size_t end = dot == std::string::npos ? prerelease.size() : dot;
        if (end == start)
            return false; // 空的标识符（开头、结尾或连续两个点）
        identifiers.push_back(prerelease.substr(start, end - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return !identifiers.empty();
}

bool AllDigits(const std::string &text)
{
    if (text.empty())
        return false;
    // 同时挡住超长数字段：下面用 long 累加，超过 18 位必然溢出，直接判为无效，
    // 而不是抛 std::out_of_range 穿透到轮询线程里把 Server 打崩。
    if (text.size() > 18)
        return false;
    for (const unsigned char ch : text)
        if (!std::isdigit(ch))
            return false;
    return true;
}

bool ParseVersionNumber(const std::string &text, long &value)
{
    // 手动累加而不是 std::stol：后者在超范围时会抛异常，而这条路径上的输入来自网络，
    // 不能让它把常驻的 Server 打崩。18 位十进制数字不可能溢出 int64。
    if (!AllDigits(text))
        return false;
    value = 0;
    for (const char ch : text)
        value = value * 10 + (ch - '0');
    return true;
}

int CompareIdentifier(const std::string &left, const std::string &right)
{
    const bool leftNumeric = AllDigits(left);
    const bool rightNumeric = AllDigits(right);
    if (leftNumeric && rightNumeric)
    {
        // 先比长度再比字典序，避免长数字溢出 long；前导零按数值处理。
        std::string a = left;
        std::string b = right;
        while (a.size() > 1 && a.front() == '0')
            a.erase(a.begin());
        while (b.size() > 1 && b.front() == '0')
            b.erase(b.begin());
        if (a.size() != b.size())
            return a.size() < b.size() ? -1 : 1;
        if (a != b)
            return a < b ? -1 : 1;
        return 0;
    }
    // 数字标识符恒小于字母标识符，这是 semver 的定义。
    if (leftNumeric != rightNumeric)
        return leftNumeric ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}
} // namespace

bool Parse(const std::string &text, Version &version)
{
    version = Version{};
    static const std::regex pattern(kVersionPattern);
    std::smatch match;
    if (!std::regex_match(text, match, pattern))
        return false;

    version.valid = true;
    const std::string core = match[1].str();
    size_t start = 0;
    while (start <= core.size())
    {
        const size_t dot = core.find('.', start);
        const std::string segment = dot == std::string::npos ? core.substr(start) : core.substr(start, dot - start);
        long number = 0;
        if (!ParseVersionNumber(segment, number))
        {
            version = Version{};
            return false;
        }
        version.parts.push_back(number);
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    version.text = core;
    if (match[2].matched)
    {
        version.prerelease = match[2].str();
        version.text += "-" + version.prerelease;
        const size_t dot = version.prerelease.find('.');
        version.label = version.prerelease.substr(0, dot);
    }
    if (version.label == "nightly")
        version.kind = Kind::Nightly;
    else if (version.label == "dev")
        version.kind = Kind::Dev;
    else if (!version.label.empty())
    {
        // 认不出的预发布标签（例如 -beta）不属于任何通道，不参与更新比较。
        version = Version{};
        return false;
    }
    else
    {
        version.kind = Kind::Release;
    }
    return true;
}

int Compare(const Version &left, const Version &right)
{
    const size_t length = (std::max)(left.parts.size(), right.parts.size());
    for (size_t i = 0; i < length; ++i)
    {
        const long l = i < left.parts.size() ? left.parts[i] : 0;
        const long r = i < right.parts.size() ? right.parts[i] : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    // 主版本相同：无预发布标签的一方更大，所以 0.7.1 > 0.7.1-nightly.…。
    if (left.prerelease.empty() != right.prerelease.empty())
        return left.prerelease.empty() ? 1 : -1;
    if (left.prerelease.empty())
        return 0;

    std::vector<std::string> a;
    std::vector<std::string> b;
    SplitPrerelease(left.prerelease, a);
    SplitPrerelease(right.prerelease, b);
    const size_t count = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < count; ++i)
    {
        const int order = CompareIdentifier(a[i], b[i]);
        if (order != 0)
            return order;
    }
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return 0;
}

bool SupportsAutoUpdate(const Version &current)
{
    return current.valid && current.kind != Kind::Dev;
}

const char *ChannelNameOf(const std::string &version)
{
    Version parsed;
    if (!Parse(version, parsed) || !SupportsAutoUpdate(parsed))
        return "";
    return parsed.kind == Kind::Nightly ? "nightly" : "release";
}

std::string TagFromReleaseUrl(const std::string &url)
{
    static const std::regex pattern(kTagPattern);
    std::smatch match;
    if (!std::regex_match(url, match, pattern))
        return {};
    return match[1].str();
}
} // namespace update_channel
