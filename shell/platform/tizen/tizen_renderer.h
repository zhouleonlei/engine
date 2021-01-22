// Copyright 2020 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EMBEDDER_TIZEN_RENDERER_H
#define EMBEDDER_TIZEN_RENDERER_H

#include <EGL/egl.h>

class TizenRenderer {
 public:
  struct TizenWindowGeometry {
    int32_t x{0}, y{0}, w{0}, h{0};
  };
  TizenRenderer() = default;
  virtual ~TizenRenderer();
  bool OnMakeCurrent();
  bool OnClearCurrent();
  bool OnMakeResourceCurrent();
  bool OnPresent();
  uint32_t OnGetFBO();
  void* OnProcResolver(const char* name);
  virtual TizenWindowGeometry GetGeometry() = 0;
  bool IsValid();
  virtual int GetEcoreWindowId() = 0;
  virtual void ResizeWithRotation(int32_t dx, int32_t dy, int32_t width,
                                  int32_t height, int32_t degree) = 0;

 protected:
  bool InitializeRenderer(int32_t x, int32_t y, int32_t w, int32_t h);
  virtual bool SetupDisplay() = 0;
  virtual bool SetupEcoreWlWindow(int32_t x, int32_t y, int32_t w,
                                  int32_t h) = 0;
  virtual bool SetupEglWindow(int32_t w, int32_t h) = 0;
  bool SetupEglSurface();
  virtual EGLDisplay GetEGLDisplay() = 0;
  virtual EGLNativeWindowType GetEGLNativeWindowType() = 0;

  void DestoryRenderer();
  void DestoryEglSurface();
  virtual void DestoryEglWindow() = 0;
  virtual void DestoryEcoreWlWindow() = 0;
  virtual void ShutdownDisplay() = 0;

 private:
  bool is_valid_ = false;
  EGLConfig egl_config_;
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;
  EGLContext egl_resource_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_resource_surface_ = EGL_NO_SURFACE;
  bool ChooseEGLConfiguration();
  void PrintEGLError();
};
#endif