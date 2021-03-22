// Copyright 2021 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tizen_renderer_evas_gl.h"

#include "flutter/shell/platform/tizen/tizen_log.h"

TizenRendererEvasGL::TizenRendererEvasGL(TizenRenderer::Delegate& delegate,
                                         int32_t x, int32_t y, int32_t w,
                                         int32_t h)
    : TizenRenderer(delegate) {
  InitializeRenderer(x, y, w, h);
}

TizenRendererEvasGL::~TizenRendererEvasGL() { DestoryRenderer(); }

void* TizenRendererEvasGL::SetupEvasWindow(int32_t x, int32_t y, int32_t w,
                                           int32_t h) {
  if (w == 0 || h == 0) {
    FT_LOGE("Failed to create because of the wrong size");
    return nullptr;
  }
  elm_config_accel_preference_set("hw:opengl");

  evas_window_ = elm_win_add(NULL, NULL, ELM_WIN_BASIC);
  elm_win_alpha_set(evas_window_, EINA_FALSE);
  elm_win_aux_hint_add(evas_window_, "wm.policy.win.user.geometry", "1");
  evas_object_move(evas_window_, x, y);
  evas_object_resize(evas_window_, w, h);
  evas_object_raise(evas_window_);

  Evas_Object* bg = elm_bg_add(evas_window_);
  evas_object_color_set(bg, 0x00, 0x00, 0x00, 0x00);

  evas_object_size_hint_weight_set(bg, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
  elm_win_resize_object_add(evas_window_, bg);

  graphics_adapter_ =
      evas_object_image_filled_add(evas_object_evas_get(evas_window_));
  evas_object_resize(graphics_adapter_, w, h);
  evas_object_move(graphics_adapter_, x, y);
  evas_object_image_size_set(graphics_adapter_, w, h);
  evas_object_image_alpha_set(graphics_adapter_, EINA_TRUE);
  elm_win_resize_object_add(evas_window_, graphics_adapter_);

  int rotations[4] = {0, 90, 180, 270};
  elm_win_wm_rotation_available_rotations_set(evas_window_,
                                              (const int*)(&rotations), 4);
  evas_object_smart_callback_add(evas_window_, "rotation,changed",
                                 RotationEventCb, this);
  return (void*)evas_window_;
}

void TizenRendererEvasGL::RotationEventCb(void* data, Evas_Object* obj,
                                          void* event_info) {
  auto* self = reinterpret_cast<TizenRendererEvasGL*>(data);
  // TODO : Use current window rotation degree
  FT_UNIMPLEMENTED();
  self->delegate_.OnRotationChange(0);
}

void TizenRendererEvasGL::Show() {
  evas_object_show((Evas_Object*)GetImageHandle());
  evas_object_show(evas_window_);
}

void TizenRendererEvasGL::SetRotate(int angle) {
  elm_win_rotation_set(evas_window_, angle);
  received_rotation = true;
}

void* TizenRendererEvasGL::GetImageHandle() { return (void*)graphics_adapter_; }

void TizenRendererEvasGL::ResizeWithRotation(int32_t x, int32_t y,
                                             int32_t width, int32_t height,
                                             int32_t angle) {
  evas_object_move(evas_window_, x, y);
  evas_object_resize(evas_window_, width, height);
  SetRotate(angle);
}

void TizenRendererEvasGL::SendRotationChangeDone() {
  elm_win_wm_rotation_manual_rotation_done(evas_window_);
}

void TizenRendererEvasGL::DestoryEvasWindow() {
  evas_object_del(evas_window_);
  evas_object_del(graphics_adapter_);
}

TizenRenderer::TizenWindowGeometry TizenRendererEvasGL::GetGeometry() {
  TizenWindowGeometry result;
  evas_object_geometry_get(evas_window_, &result.x, &result.y, &result.w,
                           &result.h);
  return result;
}

int TizenRendererEvasGL::GetEcoreWindowId() {
  return (int)ecore_evas_window_get(
      ecore_evas_ecore_evas_get(evas_object_evas_get(graphics_adapter_)));
}
