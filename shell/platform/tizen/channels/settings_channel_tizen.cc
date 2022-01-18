// Copyright 2021 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "settings_channel.h"

#include <system/system_settings.h>
#include "flutter/shell/platform/tizen/logger.h"

#if defined(TV_PROFILE)
// SYSTEM_SETTINGS_KEY_MENU_SYSTEM_ACCESSIBILITY_HIGHCONTRAST = 10059 has been defined in system_settings_keys.h only for TV profile
#define SYSTEM_SETTINGS_KEY_MENU_SYSTEM_ACCESSIBILITY_HIGHCONTRAST 10059
#endif

namespace flutter {

static void OnSettingsChangedCallback(system_settings_key_e key,
                                      void* user_data) {
  auto settings_channel = reinterpret_cast<SettingsChannel*>(user_data);
  settings_channel->SendSettingsEvent();
}

#if defined(TV_PROFILE)
static void OnAccessibilityHighContrastChangedCallback(system_settings_key_e key, void* user_data) {
  auto settings_channel = reinterpret_cast<SettingsChannel*>(user_data);
  settings_channel->OnAccessibilityHighContrastStateChanged();
}
#endif

void SettingsChannel::Init() {
  system_settings_set_changed_cb(SYSTEM_SETTINGS_KEY_LOCALE_TIMEFORMAT_24HOUR,
                                 OnSettingsChangedCallback, this);
#if defined(TV_PROFILE)
  // Add listener for accessibility high contrast
  system_settings_set_changed_cb(system_settings_key_e(SYSTEM_SETTINGS_KEY_MENU_SYSTEM_ACCESSIBILITY_HIGHCONTRAST), OnAccessibilityHighContrastChangedCallback, this);
#endif
}

void SettingsChannel::Dispose() {
  system_settings_unset_changed_cb(
      SYSTEM_SETTINGS_KEY_LOCALE_TIMEFORMAT_24HOUR);
}

bool SettingsChannel::Prefer24HourTime() {
  bool value = false;
  if (system_settings_get_value_bool(
          SYSTEM_SETTINGS_KEY_LOCALE_TIMEFORMAT_24HOUR, &value) ==
      SYSTEM_SETTINGS_ERROR_NONE) {
    return value;
  }
  return false;
}

#if defined(TV_PROFILE)
int SettingsChannel::GetAccessibilityHighContrastValue(){
  int enabled = 0;
  int ret = system_settings_get_value_int(system_settings_key_e(SYSTEM_SETTINGS_KEY_MENU_SYSTEM_ACCESSIBILITY_HIGHCONTRAST), &enabled);
  if (ret != SYSTEM_SETTINGS_ERROR_NONE) {
    FT_LOG(Error) << "Failed to get value of accessibility high contrast. ERROR CODE = "<<ret;
  }

  return enabled;
}

void SettingsChannel::SetAccessibilityHighContrastStatesHandler(const std::function<void(int)>& handler) {
  handler_ = std::move(handler);
}

void SettingsChannel::OnAccessibilityHighContrastStateChanged() {
  if (handler_) {
    handler_(GetAccessibilityHighContrastValue());
  }
}
#endif

}  // namespace flutter
