// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains the implementations of any class in the wrapper that
// - is not fully inline, and
// - is necessary for all clients of the wrapper (either app or plugin).
// It exists instead of the usual structure of having some_class_name.cc files
// so that changes to the set of things that need non-header implementations
// are not breaking changes for the template.
//
// If https://github.com/flutter/flutter/issues/57146 is fixed, this can be
// removed in favor of the normal structure since templates will no longer
// manually include files.

#include <cassert>
#include <iostream>
#include <variant>

#include "binary_messenger_impl.h"
#include "include/flutter/engine_method_result.h"
#include "texture_registrar_impl.h"

namespace flutter {

// ========== binary_messenger_impl.h ==========

namespace {
// Passes |message| to |user_data|, which must be a BinaryMessageHandler, along
// with a BinaryReply that will send a response on |message|'s response handle.
//
// This serves as an adaptor between the function-pointer-based message callback
// interface provided by the C API and the std::function-based message handler
// interface of BinaryMessenger.
void ForwardToHandler(FlutterDesktopMessengerRef messenger,
                      const FlutterDesktopMessage* message,
                      void* user_data) {
  auto* response_handle = message->response_handle;
  BinaryReply reply_handler = [messenger, response_handle](
                                  const uint8_t* reply,
                                  size_t reply_size) mutable {
    if (!response_handle) {
      std::cerr << "Error: Response can be set only once. Ignoring "
                   "duplicate response."
                << std::endl;
      return;
    }
    FlutterDesktopMessengerSendResponse(messenger, response_handle, reply,
                                        reply_size);
    // The engine frees the response handle once
    // FlutterDesktopSendMessageResponse is called.
    response_handle = nullptr;
  };

  const BinaryMessageHandler& message_handler =
      *static_cast<BinaryMessageHandler*>(user_data);

  message_handler(message->message, message->message_size,
                  std::move(reply_handler));
}
}  // namespace

BinaryMessengerImpl::BinaryMessengerImpl(
    FlutterDesktopMessengerRef core_messenger)
    : messenger_(core_messenger) {}

BinaryMessengerImpl::~BinaryMessengerImpl() = default;

void BinaryMessengerImpl::Send(const std::string& channel,
                               const uint8_t* message,
                               size_t message_size,
                               BinaryReply reply) const {
  if (reply == nullptr) {
    FlutterDesktopMessengerSend(messenger_, channel.c_str(), message,
                                message_size);
    return;
  }
  struct Captures {
    BinaryReply reply;
  };
  auto captures = new Captures();
  captures->reply = reply;

  auto message_reply = [](const uint8_t* data, size_t data_size,
                          void* user_data) {
    auto captures = reinterpret_cast<Captures*>(user_data);
    captures->reply(data, data_size);
    delete captures;
  };
  bool result = FlutterDesktopMessengerSendWithReply(
      messenger_, channel.c_str(), message, message_size, message_reply,
      captures);
  if (!result) {
    delete captures;
  }
}

void BinaryMessengerImpl::SetMessageHandler(const std::string& channel,
                                            BinaryMessageHandler handler) {
  if (!handler) {
    handlers_.erase(channel);
    FlutterDesktopMessengerSetCallback(messenger_, channel.c_str(), nullptr,
                                       nullptr);
    return;
  }
  // Save the handler, to keep it alive.
  handlers_[channel] = std::move(handler);
  BinaryMessageHandler* message_handler = &handlers_[channel];
  // Set an adaptor callback that will invoke the handler.
  FlutterDesktopMessengerSetCallback(messenger_, channel.c_str(),
                                     ForwardToHandler, message_handler);
}

// ========== engine_method_result.h ==========

namespace internal {

ReplyManager::ReplyManager(BinaryReply reply_handler)
    : reply_handler_(std::move(reply_handler)) {
  assert(reply_handler_);
}

ReplyManager::~ReplyManager() {
  if (reply_handler_) {
    // Warn, rather than send a not-implemented response, since the engine may
    // no longer be valid at this point.
    std::cerr
        << "Warning: Failed to respond to a message. This is a memory leak."
        << std::endl;
  }
}

void ReplyManager::SendResponseData(const std::vector<uint8_t>* data) {
  if (!reply_handler_) {
    std::cerr
        << "Error: Only one of Success, Error, or NotImplemented can be "
           "called,"
        << " and it can be called exactly once. Ignoring duplicate result."
        << std::endl;
    return;
  }

  const uint8_t* message = data && !data->empty() ? data->data() : nullptr;
  size_t message_size = data ? data->size() : 0;
  reply_handler_(message, message_size);
  reply_handler_ = nullptr;
}

}  // namespace internal

// ========== texture_registrar_impl.h ==========

TextureRegistrarImpl::TextureRegistrarImpl(
    FlutterDesktopTextureRegistrarRef texture_registrar_ref)
    : texture_registrar_ref_(texture_registrar_ref) {}

TextureRegistrarImpl::~TextureRegistrarImpl() = default;

int64_t TextureRegistrarImpl::RegisterTexture(TextureVariant* texture) {
  if (auto pixel_buffer_texture = std::get_if<PixelBufferTexture>(texture)) {
    FlutterDesktopTextureInfo info = {};
    info.type = kFlutterDesktopPixelBufferTexture;
    info.pixel_buffer_config.user_data = pixel_buffer_texture;
    info.pixel_buffer_config.callback =
        [](size_t width, size_t height,
           void* user_data) -> const FlutterDesktopPixelBuffer* {
      auto texture = static_cast<PixelBufferTexture*>(user_data);
      auto buffer = texture->CopyPixelBuffer(width, height);
      return buffer;
    };

    int64_t texture_id = FlutterDesktopTextureRegistrarRegisterExternalTexture(
        texture_registrar_ref_, &info);
    return texture_id;
  } else if (auto gpu_buffer_texture = std::get_if<GpuBufferTexture>(texture)) {
    FlutterDesktopTextureInfo info = {};
    info.type = kFlutterDesktopGpuBufferTexture;
    info.gpu_buffer_config.user_data = gpu_buffer_texture;
    info.gpu_buffer_config.callback =
        [](size_t width, size_t height,
           void* user_data) -> const FlutterDesktopGpuBuffer* {
      auto texture = static_cast<GpuBufferTexture*>(user_data);
      auto buffer = texture->ObtainGpuBuffer(width, height);
      return buffer;
    };

    info.gpu_buffer_config.destruction_callback = [](void* user_data) -> void {
      auto texture = static_cast<GpuBufferTexture*>(user_data);
      texture->Destruct();
    };

    int64_t texture_id = FlutterDesktopTextureRegistrarRegisterExternalTexture(
        texture_registrar_ref_, &info);
    return texture_id;
  }

  std::cerr << "Attempting to register unknown texture variant." << std::endl;
  return -1;
}  // namespace flutter

bool TextureRegistrarImpl::MarkTextureFrameAvailable(int64_t texture_id) {
  return FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
      texture_registrar_ref_, texture_id);
}

bool TextureRegistrarImpl::UnregisterTexture(int64_t texture_id) {
  return FlutterDesktopTextureRegistrarUnregisterExternalTexture(
      texture_registrar_ref_, texture_id);
}

}  // namespace flutter
