// Copyright 2020 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tizen_renderer_ecore_wl.h"

#include "flutter/shell/platform/tizen/tizen_log.h"

TizenRendererEcoreWl::TizenRendererEcoreWl(int32_t x, int32_t y, int32_t w,
                                           int32_t h) {
  InitializeRenderer(x, y, w, h);
}
TizenRendererEcoreWl::~TizenRendererEcoreWl() { DestoryRenderer(); }

bool TizenRendererEcoreWl::SetupDisplay() {
  // ecore_wl INIT
  if (!ecore_wl_init(NULL)) {
    FT_LOGE("Could not initialize ecore_wl");
    return false;
  }
  wl_display_ = ecore_wl_display_get();
  if (nullptr == wl_display_) {
    FT_LOGE("ecore_wl_display_get failed");
    return false;
  }
  return true;
}
bool TizenRendererEcoreWl::SetupEcoreWlWindow(int32_t x, int32_t y, int32_t w,
                                              int32_t h) {
  if (w == 0 || h == 0) {
    FT_LOGE("Failed to create because of the wrong size");
    return false;
  }
  ecore_wl_window_ = ecore_wl_window_new(
      nullptr, x, y, w, h, ECORE_WL_WINDOW_BUFFER_TYPE_EGL_WINDOW);
  FT_LOGD("ecore_wl_window_: %p", ecore_wl_window_);
  if (ecore_wl_window_ == nullptr) {
    FT_LOGE("ecore_wl_window_new fail");
    return false;
  }
  ecore_wl_window_type_set(ecore_wl_window_, ECORE_WL_WINDOW_TYPE_TOPLEVEL);
  // ecore_wl_window_alpha_set(ecore_wl_window_, EINA_FALSE);
  ecore_wl_window_aux_hint_add(ecore_wl_window_, 0,
                               "wm.policy.win.user.geometry", "1");
  ecore_wl_window_show(ecore_wl_window_);
  return true;
}
bool TizenRendererEcoreWl::SetupEglWindow(int32_t w, int32_t h) {
  wl_egl_window_ =
      wl_egl_window_create(ecore_wl_window_surface_get(ecore_wl_window_), w, h);
  if (nullptr == wl_egl_window_) {
    FT_LOGE("wl_egl_window_create is Failed");
    return false;
  }
  return true;
}
EGLDisplay TizenRendererEcoreWl::GetEGLDisplay() {
  return eglGetDisplay((EGLNativeDisplayType)wl_display_);
}
EGLNativeWindowType TizenRendererEcoreWl::GetEGLNativeWindowType() {
  return (EGLNativeWindowType)wl_egl_window_;
}

void TizenRendererEcoreWl::DestoryEglWindow() {
  if (wl_egl_window_) {
    wl_egl_window_destroy(wl_egl_window_);
    wl_egl_window_ = nullptr;
  }
}
void TizenRendererEcoreWl::DestoryEcoreWlWindow() {
  if (ecore_wl_window_) {
    ecore_wl_window_free(ecore_wl_window_);
    ecore_wl_window_ = nullptr;
  }
}
void TizenRendererEcoreWl::ShutdownDisplay() { ecore_wl_shutdown(); }

TizenRenderer::TizenWindowGeometry TizenRendererEcoreWl::GetGeometry() {
  TizenWindowGeometry result;
  ecore_wl_window_geometry_get(ecore_wl_window_, &result.x, &result.y,
                               &result.w, &result.h);
  return result;
}

void TizenRendererEcoreWl::ResizeWithRotation(int32_t dx, int32_t dy,
                                              int32_t width, int32_t height,
                                              int32_t degree) {
  wl_egl_window_resize(wl_egl_window_, width, height, dx, dy);
  wl_egl_window_rotation rotations[4] = {ROTATION_0, ROTATION_90, ROTATION_180,
                                         ROTATION_270};
  int index = 0;
  if (degree > 0) {
    index = (degree / 90) % 4;
  }
  wl_egl_window_set_rotation(wl_egl_window_, rotations[index]);
}

int TizenRendererEcoreWl::GetEcoreWindowId() {
  return ecore_wl_window_id_get(ecore_wl_window_);
}