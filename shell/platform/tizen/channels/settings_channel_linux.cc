// Copyright 2021 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "settings_channel.h"

namespace flutter {

void SettingsChannel::Init() {}

void SettingsChannel::Dispose() {}

bool SettingsChannel::Prefer24HourTime() {
  return false;
}
#if defined(TV_PROFILE)
int SettingsChannel::GetAccessibilityHighContrastValue(){};
void SettingsChannel::OnAccessibilityHighContrastStateChanged(){};
void SettingsChannel::SetAccessibilityHighContrastStatesHandler(const std::function<void(int)>& handler){};
#endif
}  // namespace flutter
