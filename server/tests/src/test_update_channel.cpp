#include "tests/includes/test_framework.h"
#include "tests/includes/test_utf8_path.h"

#include "settings/update_channel.h"
#include "settings/update_download.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
using update_channel::Compare;
using update_channel::Kind;
using update_channel::Version;

Version Parsed(const std::string &text)
{
    Version version;
    REQUIRE(update_channel::Parse(text, version));
    return version;
}
} // namespace

TEST_CASE(update_channel_classifies_each_release_line)
{
    // 正式版没有任何预发布标签；nightly 和 dev 由 dev-build-version.py 生成，形如
    // MAJOR.MINOR.PATCH-nightly.YYYYMMDD.<短提交>。
    REQUIRE(Parsed("0.7.1").kind == Kind::Release);
    REQUIRE(Parsed("v0.7.1").kind == Kind::Release);
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").kind == Kind::Nightly);
    REQUIRE(Parsed("0.7.1-dev.20261022.7fa6fb1a").kind == Kind::Dev);
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").prerelease == "nightly.20261022.7fa6fb1a");
    REQUIRE(Parsed("0.7.1-nightly.20261022.7fa6fb1a").label == "nightly");

    // 构建元数据不参与比较。
    REQUIRE(Parsed("0.7.1+build.5").text == "0.7.1");
}

TEST_CASE(update_channel_rejects_malformed_versions)
{
    Version version;
    REQUIRE(!update_channel::Parse("", version));
    REQUIRE(!version.valid);
    REQUIRE(!update_channel::Parse("abc", version));
    REQUIRE(!update_channel::Parse("0.x.1", version));
    REQUIRE(!update_channel::Parse("0.7.1-", version));
}

TEST_CASE(update_channel_orders_versions_inside_one_line)
{
    // 正式版高于同号的预发布版本，这是 semver 的定义，也是「nightly 不会把自己认成比正式版新」
    // 的前提：nightly 只和 nightly 比，正式版只和正式版比。
    REQUIRE(Compare(Parsed("0.7.1"), Parsed("0.7.0")) > 0);
    REQUIRE(Compare(Parsed("0.7.1"), Parsed("0.7.1-nightly.20261022.7fa6fb1a")) > 0);
    REQUIRE(Compare(Parsed("0.8.0"), Parsed("0.7.9")) > 0);
    // 三段补齐：0.7 等价于 0.7.0。
    REQUIRE(Compare(Parsed("0.7"), Parsed("0.7.0")) == 0);

    const auto nightly_a = Parsed("0.7.1-nightly.20261022.7fa6fb1a");
    const auto nightly_b = Parsed("0.7.1-nightly.20261023.abcd1234");
    REQUIRE(Compare(nightly_b, nightly_a) > 0);
    // 日期相同就比提交，数字段按数值比较，与字符串长度无关。
    REQUIRE(Compare(Parsed("0.7.1-nightly.20261022.b"), Parsed("0.7.1-nightly.20261022.aa")) > 0);
}

TEST_CASE(update_channel_auto_update_only_runs_on_release_lines)
{
    // dev 构建永远不参与自动更新：它的版本号每天变，比较结果没有意义。
    REQUIRE(update_channel::SupportsAutoUpdate(Parsed("0.7.1")));
    REQUIRE(update_channel::SupportsAutoUpdate(Parsed("0.7.1-nightly.20261022.7fa6fb1a")));
    REQUIRE(!update_channel::SupportsAutoUpdate(Parsed("0.7.1-dev.20261022.7fa6fb1a")));
    Version invalid;
    REQUIRE(!update_channel::SupportsAutoUpdate(invalid));

    // 认不出的预发布标签不属于任何通道，不能被当成正式版参与自动下载。
    Version beta;
    REQUIRE(!update_channel::Parse("0.7.1-beta.3", beta));
    REQUIRE(!update_channel::SupportsAutoUpdate(beta));

    // 界面用这个名字决定渲染不渲染勾选框。
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1")), std::string("release"));
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-nightly.20261022.7fa6fb1a")), std::string("nightly"));
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-dev.20261022.7fa6fb1a")), std::string());
    REQUIRE_EQ(std::string(update_channel::ChannelNameOf("0.7.1-beta.3")), std::string());
}

TEST_CASE(update_channel_survives_absurd_version_numbers)
{
    // 清单来自网络。超长数字段过去会让 std::stol 抛 out_of_range，穿透到轮询线程里
    // 把常驻的 Server 打崩；现在必须只是「解析失败」。
    Version version;
    REQUIRE(!update_channel::Parse("999999999999999999999999.1", version));
    REQUIRE(!update_channel::Parse("1.99999999999999999999999.1", version));
    REQUIRE(update_channel::Parse("20261022.7.1", version));
    REQUIRE(version.parts.size() == 3);
}

TEST_CASE(update_channel_reads_the_tag_out_of_a_release_url)
{
    REQUIRE(update_channel::TagFromReleaseUrl("https://github.com/metasequoiaime/MSIME-Windows/releases/tag/v0.7.1") ==
            "v0.7.1");
    REQUIRE(update_channel::TagFromReleaseUrl(
                "https://github.com/metasequoiaime/MSIME-Windows/releases/tag/0.7.1-nightly.20261022.7fa6fb1a") ==
            "0.7.1-nightly.20261022.7fa6fb1a");
    // 别的仓库、别的路径形状一律不认。
    REQUIRE(update_channel::TagFromReleaseUrl("https://example.com/releases/tag/v1").empty());
    REQUIRE(update_channel::TagFromReleaseUrl("https://github.com/metasequoiaime/x/releases/latest").empty());
}

TEST_CASE(update_download_guards_the_file_names_it_writes)
{
    REQUIRE(update_download::IsInstallerName("MetasequoiaIME_Setup_v0.7.1.exe"));
    REQUIRE(update_download::IsInstallerName("MetasequoiaIME_Setup_v0.7.1-dev.20261022.7fa6fb1a.exe"));
    REQUIRE(!update_download::IsInstallerName("setup.exe"));
    REQUIRE(!update_download::IsInstallerName("MetasequoiaIME_Setup_v0.7.1.exe.part"));
    REQUIRE(!update_download::IsInstallerName("MetasequoiaIME_Setup_v0.7.1.zip"));

    // 摘要必须正好 64 个小写十六进制字符，别的形式一律拒绝自动下载。
    REQUIRE(update_download::IsSha256Hex(std::string(64, 'a')));
    REQUIRE(!update_download::IsSha256Hex(std::string(63, 'a')));
    REQUIRE(!update_download::IsSha256Hex(std::string(64, 'A')));
    REQUIRE(!update_download::IsSha256Hex(std::string(64, 'g')));
}

TEST_CASE(update_download_maps_github_assets_to_the_mirror)
{
    const std::string github =
        "https://github.com/metasequoiaime/msime-windows/releases/download/v0.7.1/MetasequoiaIME_Setup_v0.7.1.exe";
    REQUIRE(update_download::MirrorUrlFor(github) == "https://dl.msime.app/gh/" + github);
    // 不是资产直链就不映射，避免把任意地址拼到镜像前缀后面。
    REQUIRE(update_download::MirrorUrlFor("https://github.com/metasequoiaime/x/releases/tag/v0.7.1").empty());
    REQUIRE(update_download::MirrorUrlFor("update.json").empty());
}

TEST_CASE(update_download_keeps_download_state_in_the_data_directory)
{
    namespace fs = std::filesystem;
    const fs::path root = test::TempRoot() / L"update-download-status";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    REQUIRE(!ec);

    // 状态文件位置由数据目录决定；测试里把它指向临时目录，避免碰到真实安装的数据目录。
    const std::wstring previous = [&] {
        DWORD needed = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
        std::wstring value(needed, L'\0');
        if (needed != 0)
            GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", value.data(), needed);
        if (needed != 0 && !value.empty() && value.back() == L'\0')
            value.pop_back();
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", root.c_str());
        return value;
    }();

    const fs::path status = update_download::StatusPath();
    REQUIRE(!status.empty());
    REQUIRE(status.parent_path() == root / L"updates");

    // 没有文件时读到的是 Idle，而不是上一次的错误。
    const update_download::Progress idle = update_download::ReadStatus();
    REQUIRE(idle.state == update_download::State::Idle);

    update_download::Progress written;
    written.state = update_download::State::Ready;
    written.version = "0.7.1";
    written.installer = "MetasequoiaIME_Setup_v0.7.1.exe";
    update_download::WriteStatus(written);
    const update_download::Progress read_back = update_download::ReadStatus();
    REQUIRE(read_back.state == update_download::State::Ready);
    REQUIRE_EQ(read_back.version, std::string("0.7.1"));
    REQUIRE_EQ(read_back.installer, std::string("MetasequoiaIME_Setup_v0.7.1.exe"));

    // 只有状态说「就绪」不算数：InstallPathFor 只信文件系统。
    REQUIRE(update_download::InstallerPathFor("0.7.1", written.installer).empty());
    // 版本号里的路径穿越在拼目录之前就被挡掉。
    REQUIRE(update_download::InstallerPathFor("..\\..\\windows", written.installer).empty());
    REQUIRE(update_download::InstallerPathFor("../..", written.installer).empty());

    // 真的把文件放到位之后才认。
    const fs::path directory = root / L"updates" / L"0.7.1";
    fs::create_directories(directory, ec);
    REQUIRE(!ec);
    const fs::path installer = directory / L"MetasequoiaIME_Setup_v0.7.1.exe";
    {
        std::ofstream file(installer, std::ios::binary);
        file << "not a real installer";
    }
    REQUIRE(fs::is_regular_file(installer));
    REQUIRE(update_download::InstallerPathFor("0.7.1", written.installer) == installer);

    // 摘要格式不合法时 Download 必须直接拒绝，不做任何网络请求：清单没给校验值
    // 就等于放弃完整性判断。真正的网络下载不进 CI，这里只验这道门槛。
    const update_download::Result rejected =
        update_download::Download("0.7.1", written.installer, "https://example.com/x.exe", "", "not-a-digest", {});
    REQUIRE(rejected.state == update_download::State::Failed);
    REQUIRE(!rejected.error.empty());

    SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
    fs::remove_all(root, ec);
}
