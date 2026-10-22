#pragma once

#include <string>

bool OpenSettingsApplication();
bool OpenSettingsAboutApplication();
bool CloseSettingsApplication();
bool OpenEmojiPanelApplication();
bool CloseEmojiPanelApplication();
bool OpenKeyboardPanelApplication();
bool CloseKeyboardPanelApplication();
bool OpenHandwritingPanelApplication();
bool CloseHandwritingPanelApplication();
// 拉起自动下载并校验过的安装包。路径来自服务端写下的状态，这里仍重新校验命名与存在性。
bool LaunchUpdateInstaller(const std::string &installer_path);
