// Copyright 2021 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EMBEDDER_TIZEN_RENDERER_EVAS_GL_H
#define EMBEDDER_TIZEN_RENDERER_EVAS_GL_H

#include "tizen_renderer.h"

#include <Elementary.h>
#include <Evas_GL.h>

class TizenRendererEvasGL : public TizenRenderer {
 public:
  TizenRendererEvasGL(TizenRenderer::Delegate &delegate, int32_t x, int32_t y,
                        int32_t w, int32_t h);
  ~TizenRendererEvasGL();
  TizenWindowGeometry GetGeometry() override;
  int GetEcoreWindowId() override;
  void ResizeWithRotation(int32_t x, int32_t y, int32_t width, int32_t height,
                          int32_t angle) override;
  void Show() override;
  void SetRotate(int angle) override;

 protected:
  virtual void* SetupEvasWindow(int32_t x, int32_t y, int32_t w,
                                  int32_t h) override;  
  virtual void DestoryEvasWindow() override;
  void SendRotationChangeDone() override;
  void* GetImageHandle() override;
 private:
  Evas_Object* evas_window_{nullptr};
  Evas_Object* graphicsAdapter_{nullptr};
  static void RotationEventCb(void *data, Evas_Object * obj,
									   void *event_info);
};
#endif //EMBEDDER_TIZEN_RENDERER_ECORE_WL2_H
