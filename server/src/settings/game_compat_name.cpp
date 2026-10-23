#include "game_compat_name.h"

#include <shellapi.h>

#include <vector>

namespace game_compat_name
{
namespace
{

constexpr wchar_t kApplySwitch[] = L"--apply-game-compat-name";
constexpr wchar_t kClearSwitch[] = L"--clear-game-compat-name";

// Read by windows/src/Register/Register.cpp at registration time; keep the two in step.
constexpr wchar_t kRecordKey[] = L"Software\\Metasequoia\\MetasequoiaIME";
constexpr wchar_t kRecordValue[] = L"GameCompatProfileName";
// Where windows/src/Register/Register.cpp registers the profile: the profile GUID below the CLSID, with the LANGID as
// the subkey. This is also the key GetLanguageProfileDescription and the language bar read (Display Description if
// present, Description otherwise), and the only place a TSF profile name lives; there is no second copy under
// SOFTWARE\Microsoft\CTF\TIP in a separate layout.
constexpr wchar_t kTipProfileKey[] = L"SOFTWARE\\Microsoft\\CTF\\TIP\\{E3062E9A-D834-4637-8958-ED8CFA427D01}"
                                     L"\\LanguageProfile\\0x00000804\\{4D59B1B4-D503-44AE-9259-BAD9BB2778AB}";
// TEXTSERVICE_DESC in windows/src/Register/Register.cpp: what the profile is called with the switch off.
constexpr wchar_t kProductName[] = L"Metasequoia 水杉输入法";

enum ExitCode : int
{
    Ok = 0,
    BadArguments = 2,
    RecordFailed = 3,
    NotRegistered = 4,
};

std::wstring ReadString(HKEY root, const std::wstring &subkey, const wchar_t *value)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer.data(),
                     &bytes) != ERROR_SUCCESS)
        return {};
    return std::wstring(buffer.data());
}

bool WriteString(HKEY root, const std::wstring &subkey, const wchar_t *value, const std::wstring &data, bool createKey)
{
    HKEY key = nullptr;
    const LSTATUS opened = createKey ? RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0,
                                                       KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr)
                                     : RegOpenKeyExW(root, subkey.c_str(), 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS)
        return false;
    const LSTATUS stored = RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return stored == ERROR_SUCCESS;
}

bool KeyExists(HKEY root, const std::wstring &subkey)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

bool IsTipRegistered()
{
    return KeyExists(HKEY_LOCAL_MACHINE, kTipProfileKey);
}

// Name of the profile as CTF has it now. Empty when the profile has no name at all.
std::wstring CurrentRegisteredName()
{
    return ReadString(HKEY_LOCAL_MACHINE, kTipProfileKey, L"Description");
}

int ApplyElevated(bool enabled, const std::wstring &name)
{
    if (enabled && !IsValidName(name))
        return BadArguments;
    if (!IsTipRegistered())
        return NotRegistered;
    if (enabled)
    {
        if (!WriteString(HKEY_LOCAL_MACHINE, kRecordKey, kRecordValue, name, true))
            return RecordFailed;
    }
    else
    {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kRecordKey, kRecordValue);
        // RegDeleteKeyValue has no WOW64 flag; this process is 64-bit, so it already sees the native view.
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
            return RecordFailed;
    }
    // The TSF DLL writes the same key with RegisterProfile on the next install; this covers the machine whose
    // registration predates the switch, so the name check at the end of Apply is what decides whether it took.
    const std::wstring target = enabled ? name : std::wstring(kProductName);
    WriteString(HKEY_LOCAL_MACHINE, kTipProfileKey, L"Description", target, false);
    return Ok;
}

std::string DescribeExitCode(DWORD code)
{
    switch (code)
    {
    case BadArguments:
        return "名称无效：请填写 1 到 64 个字符，不能包含双引号或反斜杠。";
    case RecordFailed:
        return "无法写入设置，请确认在弹出的授权窗口中选择了「是」。";
    case NotRegistered:
        return "水杉输入法没有在系统中注册，请重新安装输入法。";
    default:
        return "应用失败（错误码 " + std::to_string(code) + "）。";
    }
}

std::wstring OwnExecutablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
            return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace

bool IsValidName(const std::wstring &name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (const wchar_t ch : name)
    {
        // Quotes and backslashes would break the quoting of the elevated command line.
        if (ch < 0x20 || ch == 0x7f || ch == L'"' || ch == L'\\')
            return false;
    }
    return true;
}

Status QueryStatus()
{
    Status status;
    status.name = ReadString(HKEY_LOCAL_MACHINE, kRecordKey, kRecordValue);
    status.enabled = !status.name.empty();
    status.registeredName = CurrentRegisteredName();
    // The record is gone but the profile still carries a name of its own. Only RegisterProfile writes that key on an
    // elevated registration, so switching off cannot undo it; the next install or upgrade does.
    status.reregister_pending =
        !status.enabled && !status.registeredName.empty() && status.registeredName != kProductName;
    return status;
}

bool Apply(HWND owner, bool enabled, const std::wstring &name, std::string &error)
{
    if (enabled && !IsValidName(name))
    {
        error = DescribeExitCode(BadArguments);
        return false;
    }
    const std::wstring executable = OwnExecutablePath();
    if (executable.empty())
    {
        error = "找不到设置程序自身的路径。";
        return false;
    }
    const std::wstring parameters =
        enabled ? std::wstring(kApplySwitch) + L" \"" + name + L"\"" : std::wstring(kClearSwitch);

    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.hwnd = owner;
    execute.lpVerb = L"runas";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess)
    {
        error = GetLastError() == ERROR_CANCELLED ? "已取消：修改登记名称需要管理员权限。"
                                                  : "无法以管理员身份运行设置程序。";
        return false;
    }
    const DWORD wait = WaitForSingleObject(execute.hProcess, 60 * 1000);
    DWORD code = static_cast<DWORD>(-1);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(execute.hProcess, &code);
    CloseHandle(execute.hProcess);
    if (wait != WAIT_OBJECT_0)
    {
        error = "等待管理员权限的操作超时。";
        return false;
    }
    if (code != Ok)
    {
        error = DescribeExitCode(code);
        return false;
    }
    const Status after = QueryStatus();
    if (after.registeredName != (enabled ? name : std::wstring(kProductName)))
    {
        error = "系统没有应用新的登记名称。";
        return false;
    }
    return true;
}

int RunElevatedApplyIfRequested(const wchar_t *commandLine)
{
    if (commandLine == nullptr || *commandLine == L'\0')
        return -1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(commandLine, &argc);
    if (argv == nullptr)
        return -1;
    int result = -1;
    if (argc >= 1 && wcscmp(argv[0], kClearSwitch) == 0)
        result = ApplyElevated(false, {});
    else if (argc >= 2 && wcscmp(argv[0], kApplySwitch) == 0)
        result = ApplyElevated(true, argv[1]);
    else if (argc >= 1 && wcscmp(argv[0], kApplySwitch) == 0)
        result = BadArguments;
    LocalFree(argv);
    return result;
}

} // namespace game_compat_name
