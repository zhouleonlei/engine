// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include <algorithm>
#include <ctime>
#include <functional>
#include <future>
#include <memory>
#include <vector>

#include "assets/directory_asset_bundle.h"
#include "common/graphics/persistent_cache.h"
#include "flutter/common/graphics/persistent_cache.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/picture_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/fml/command_line.h"
#include "flutter/fml/dart/dart_converter.h"
#include "flutter/fml/make_copyable.h"
#include "flutter/fml/message_loop.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/runtime/dart_vm.h"
#include "flutter/shell/common/platform_view.h"
#include "flutter/shell/common/rasterizer.h"
#include "flutter/shell/common/shell_test.h"
#include "flutter/shell/common/shell_test_external_view_embedder.h"
#include "flutter/shell/common/shell_test_platform_view.h"
#include "flutter/shell/common/switches.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/shell/common/vsync_waiter_fallback.h"
#include "flutter/shell/version/version.h"
#include "flutter/testing/testing.h"
#include "gmock/gmock.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/tonic/converter/dart_converter.h"

#ifdef SHELL_ENABLE_VULKAN
#include "flutter/vulkan/vulkan_application.h"  // nogncheck
#endif

namespace flutter {
namespace testing {

using ::testing::_;
using ::testing::Return;

namespace {
class MockPlatformViewDelegate : public PlatformView::Delegate {
  MOCK_METHOD1(OnPlatformViewCreated, void(std::unique_ptr<Surface> surface));

  MOCK_METHOD0(OnPlatformViewDestroyed, void());

  MOCK_METHOD1(OnPlatformViewSetNextFrameCallback,
               void(const fml::closure& closure));

  MOCK_METHOD1(OnPlatformViewSetViewportMetrics,
               void(const ViewportMetrics& metrics));

  MOCK_METHOD1(OnPlatformViewDispatchPlatformMessage,
               void(std::unique_ptr<PlatformMessage> message));

  MOCK_METHOD1(OnPlatformViewDispatchPointerDataPacket,
               void(std::unique_ptr<PointerDataPacket> packet));

  MOCK_METHOD2(OnPlatformViewDispatchKeyDataPacket,
               void(std::unique_ptr<KeyDataPacket> packet,
                    KeyDataResponse callback));

  MOCK_METHOD3(OnPlatformViewDispatchSemanticsAction,
               void(int32_t id,
                    SemanticsAction action,
                    fml::MallocMapping args));

  MOCK_METHOD1(OnPlatformViewSetSemanticsEnabled, void(bool enabled));

  MOCK_METHOD1(OnPlatformViewSetAccessibilityFeatures, void(int32_t flags));

  MOCK_METHOD1(OnPlatformViewRegisterTexture,
               void(std::shared_ptr<Texture> texture));

  MOCK_METHOD1(OnPlatformViewUnregisterTexture, void(int64_t texture_id));

  MOCK_METHOD1(OnPlatformViewMarkTextureFrameAvailable,
               void(int64_t texture_id));

  MOCK_METHOD3(LoadDartDeferredLibrary,
               void(intptr_t loading_unit_id,
                    std::unique_ptr<const fml::Mapping> snapshot_data,
                    std::unique_ptr<const fml::Mapping> snapshot_instructions));

  MOCK_METHOD3(LoadDartDeferredLibraryError,
               void(intptr_t loading_unit_id,
                    const std::string error_message,
                    bool transient));

  MOCK_METHOD2(UpdateAssetResolverByType,
               void(std::unique_ptr<AssetResolver> updated_asset_resolver,
                    AssetResolver::AssetResolverType type));
};

class MockSurface : public Surface {
  MOCK_METHOD0(IsValid, bool());

  MOCK_METHOD1(AcquireFrame,
               std::unique_ptr<SurfaceFrame>(const SkISize& size));

  MOCK_CONST_METHOD0(GetRootTransformation, SkMatrix());

  MOCK_METHOD0(GetContext, GrDirectContext*());

  MOCK_METHOD0(MakeRenderContextCurrent, std::unique_ptr<GLContextResult>());

  MOCK_METHOD0(ClearRenderContext, bool());
};

class MockPlatformView : public PlatformView {
 public:
  MockPlatformView(MockPlatformViewDelegate& delegate, TaskRunners task_runners)
      : PlatformView(delegate, task_runners) {}
  MOCK_METHOD0(CreateRenderingSurface, std::unique_ptr<Surface>());
  MOCK_CONST_METHOD0(GetPlatformMessageHandler,
                     std::shared_ptr<PlatformMessageHandler>());
};

class MockPlatformMessageHandler : public PlatformMessageHandler {
 public:
  MOCK_METHOD1(HandlePlatformMessage,
               void(std::unique_ptr<PlatformMessage> message));
  MOCK_METHOD2(InvokePlatformMessageResponseCallback,
               void(int response_id, std::unique_ptr<fml::Mapping> mapping));
  MOCK_METHOD1(InvokePlatformMessageEmptyResponseCallback,
               void(int response_id));
};

class MockPlatformMessageResponse : public PlatformMessageResponse {
 public:
  static fml::RefPtr<MockPlatformMessageResponse> Create() {
    return fml::AdoptRef(new MockPlatformMessageResponse());
  }
  MOCK_METHOD1(Complete, void(std::unique_ptr<fml::Mapping> data));
  MOCK_METHOD0(CompleteEmpty, void());
};
}  // namespace

class TestAssetResolver : public AssetResolver {
 public:
  TestAssetResolver(bool valid, AssetResolver::AssetResolverType type)
      : valid_(valid), type_(type) {}

  bool IsValid() const override { return true; }

  // This is used to identify if replacement was made or not.
  bool IsValidAfterAssetManagerChange() const override { return valid_; }

  AssetResolver::AssetResolverType GetType() const override { return type_; }

  std::unique_ptr<fml::Mapping> GetAsMapping(
      const std::string& asset_name) const override {
    return nullptr;
  }

  std::vector<std::unique_ptr<fml::Mapping>> GetAsMappings(
      const std::string& asset_pattern,
      const std::optional<std::string>& subdir) const override {
    return {};
  };

 private:
  bool valid_;
  AssetResolver::AssetResolverType type_;
};

static bool ValidateShell(Shell* shell) {
  if (!shell) {
    return false;
  }

  if (!shell->IsSetup()) {
    return false;
  }

  ShellTest::PlatformViewNotifyCreated(shell);

  {
    fml::AutoResetWaitableEvent latch;
    fml::TaskRunner::RunNowOrPostTask(
        shell->GetTaskRunners().GetPlatformTaskRunner(), [shell, &latch]() {
          shell->GetPlatformView()->NotifyDestroyed();
          latch.Signal();
        });
    latch.Wait();
  }

  return true;
}

static bool RasterizerHasLayerTree(Shell* shell) {
  fml::AutoResetWaitableEvent latch;
  bool has_layer_tree = false;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(),
      [shell, &latch, &has_layer_tree]() {
        has_layer_tree = shell->GetRasterizer()->GetLastLayerTree() != nullptr;
        latch.Signal();
      });
  latch.Wait();
  return has_layer_tree;
}

static void ValidateDestroyPlatformView(Shell* shell) {
  ASSERT_TRUE(shell != nullptr);
  ASSERT_TRUE(shell->IsSetup());

  // To validate destroy platform view, we must ensure the rasterizer has a
  // layer tree before the platform view is destroyed.
  ASSERT_TRUE(RasterizerHasLayerTree(shell));

  ShellTest::PlatformViewNotifyDestroyed(shell);
  // Validate the layer tree is destroyed
  ASSERT_FALSE(RasterizerHasLayerTree(shell));
}

static std::string CreateFlagsString(std::vector<const char*>& flags) {
  if (flags.size() == 0) {
    return "";
  }
  std::string flags_string = flags[0];
  for (size_t i = 1; i < flags.size(); ++i) {
    flags_string += ",";
    flags_string += flags[i];
  }
  return flags_string;
}

static void TestDartVmFlags(std::vector<const char*>& flags) {
  std::string flags_string = CreateFlagsString(flags);
  const std::vector<fml::CommandLine::Option> options = {
      fml::CommandLine::Option("dart-flags", flags_string)};
  fml::CommandLine command_line("", options, std::vector<std::string>());
  flutter::Settings settings = flutter::SettingsFromCommandLine(command_line);
  EXPECT_EQ(settings.dart_flags.size(), flags.size());
  for (size_t i = 0; i < flags.size(); ++i) {
    EXPECT_EQ(settings.dart_flags[i], flags[i]);
  }
}

static void PostSync(const fml::RefPtr<fml::TaskRunner>& task_runner,
                     const fml::closure& task) {
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(task_runner, [&latch, &task] {
    task();
    latch.Signal();
  });
  latch.Wait();
}

TEST_F(ShellTest, InitializeWithInvalidThreads) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  TaskRunners task_runners("test", nullptr, nullptr, nullptr, nullptr);
  auto shell = CreateShell(std::move(settings), std::move(task_runners));
  ASSERT_FALSE(shell);
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithDifferentThreads) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform | ThreadHost::Type::RASTER |
                             ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  auto shell = CreateShell(std::move(settings), std::move(task_runners));
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithSingleThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));
  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithSingleThreadWhichIsTheCallingThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest,
       InitializeWithMultipleThreadButCallingThreadAsPlatformThread) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host(
      "io.flutter.test." + GetCurrentTestName() + ".",
      ThreadHost::Type::RASTER | ThreadHost::Type::IO | ThreadHost::Type::UI);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  auto shell = Shell::Create(
      flutter::PlatformData(), std::move(task_runners), settings,
      [](Shell& shell) {
        // This is unused in the platform view as we are not using the simulated
        // vsync mechanism. We should have better DI in the tests.
        const auto vsync_clock = std::make_shared<ShellTestVsyncClock>();
        return ShellTestPlatformView::Create(
            shell, shell.GetTaskRunners(), vsync_clock,
            [task_runners = shell.GetTaskRunners()]() {
              return static_cast<std::unique_ptr<VsyncWaiter>>(
                  std::make_unique<VsyncWaiterFallback>(task_runners));
            },
            ShellTestPlatformView::BackendType::kDefaultBackend, nullptr);
      },
      [](Shell& shell) { return std::make_unique<Rasterizer>(shell); });
  ASSERT_TRUE(ValidateShell(shell.get()));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithDisabledGpu) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(
      std::move(settings), std::move(task_runners), /*simulate_vsync=*/false,
      /*shell_test_external_view_embedder=*/nullptr, /*is_gpu_disabled=*/true);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  bool is_disabled = false;
  shell->GetIsGpuDisabledSyncSwitch()->Execute(
      fml::SyncSwitch::Handlers().SetIfTrue([&] { is_disabled = true; }));
  ASSERT_TRUE(is_disabled);

  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, InitializeWithGPUAndPlatformThreadsTheSame) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host(
      "io.flutter.test." + GetCurrentTestName() + ".",
      ThreadHost::Type::Platform | ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners(
      "test",
      thread_host.platform_thread->GetTaskRunner(),  // platform
      thread_host.platform_thread->GetTaskRunner(),  // raster
      thread_host.ui_thread->GetTaskRunner(),        // ui
      thread_host.io_thread->GetTaskRunner()         // io
  );
  auto shell = CreateShell(std::move(settings), std::move(task_runners));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));
  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, FixturesAreFunctional) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("fixturesAreFunctionalMain");

  fml::AutoResetWaitableEvent main_latch;
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain",
      CREATE_NATIVE_ENTRY([&main_latch](auto args) { main_latch.Signal(); }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, SecondaryIsolateBindingsAreSetupViaShellSettings) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("testCanLaunchSecondaryIsolate");

  fml::CountDownLatch latch(2);
  AddNativeCallback("NotifyNative", CREATE_NATIVE_ENTRY([&latch](auto args) {
                      latch.CountDown();
                    }));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();

  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, LastEntrypoint) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  std::string entry_point = "fixturesAreFunctionalMain";
  configuration.SetEntrypoint(entry_point);

  fml::AutoResetWaitableEvent main_latch;
  std::string last_entry_point;
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain", CREATE_NATIVE_ENTRY([&](auto args) {
        last_entry_point = shell->GetEngine()->GetLastEntrypoint();
        main_latch.Signal();
      }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  EXPECT_EQ(entry_point, last_entry_point);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_DisallowedDartVMFlag
#else
       DisallowedDartVMFlag
#endif  // defined(WINUWP)
) {
  // Run this test in a thread-safe manner, otherwise gtest will complain.
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";

  const std::vector<fml::CommandLine::Option> options = {
      fml::CommandLine::Option("dart-flags", "--verify_after_gc")};
  fml::CommandLine command_line("", options, std::vector<std::string>());

  // Upon encountering a disallowed Dart flag the process terminates.
  const char* expected =
      "Encountered disallowed Dart VM flag: --verify_after_gc";
  ASSERT_DEATH(flutter::SettingsFromCommandLine(command_line), expected);
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_AllowedDartVMFlag
#else
       AllowedDartVMFlag
#endif  // defined(WINUWP)
) {
  std::vector<const char*> flags = {
      "--enable-isolate-groups",
      "--no-enable-isolate-groups",
      "--lazy_async_stacks",
  };
#if !FLUTTER_RELEASE
  flags.push_back("--max_profile_depth 1");
  flags.push_back("--random_seed 42");
  if (!DartVM::IsRunningPrecompiledCode()) {
    flags.push_back("--enable_mirrors");
  }
#endif

  TestDartVmFlags(flags);
}

TEST_F(ShellTest, NoNeedToReportTimingsByDefault) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  ASSERT_FALSE(GetNeedsReportTimings(shell.get()));

  // This assertion may or may not be the direct result of needs_report_timings_
  // being false. The count could be 0 simply because we just cleared
  // unreported timings by reporting them. Hence this can't replace the
  // ASSERT_FALSE(GetNeedsReportTimings(shell.get())) check. We added
  // this assertion for an additional confidence that we're not pushing
  // back to unreported timings unnecessarily.
  //
  // Conversely, do not assert UnreportedTimingsCount(shell.get()) to be
  // positive in any tests. Otherwise those tests will be flaky as the clearing
  // of unreported timings is unpredictive.
  ASSERT_EQ(UnreportedTimingsCount(shell.get()), 0);
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, NeedsReportTimingsIsSetWithCallback) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("dummyReportTimingsMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  ASSERT_TRUE(GetNeedsReportTimings(shell.get()));
  DestroyShell(std::move(shell));
}

static void CheckFrameTimings(const std::vector<FrameTiming>& timings,
                              fml::TimePoint start,
                              fml::TimePoint finish) {
  fml::TimePoint last_frame_start;
  for (size_t i = 0; i < timings.size(); i += 1) {
    // Ensure that timings are sorted.
    ASSERT_TRUE(timings[i].Get(FrameTiming::kPhases[0]) >= last_frame_start);
    last_frame_start = timings[i].Get(FrameTiming::kPhases[0]);

    fml::TimePoint last_phase_time;
    for (auto phase : FrameTiming::kPhases) {
      // raster finish wall time doesn't use the same clock base
      // as rest of the frame timings.
      if (phase == FrameTiming::kRasterFinishWallTime) {
        continue;
      }

      ASSERT_TRUE(timings[i].Get(phase) >= start);
      ASSERT_TRUE(timings[i].Get(phase) <= finish);

      // phases should have weakly increasing time points
      ASSERT_TRUE(last_phase_time <= timings[i].Get(phase));
      last_phase_time = timings[i].Get(phase);
    }
  }
}

TEST_F(ShellTest, ReportTimingsIsCalled) {
  fml::TimePoint start = fml::TimePoint::Now();
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("reportTimingsMain");
  fml::AutoResetWaitableEvent reportLatch;
  std::vector<int64_t> timestamps;
  auto nativeTimingCallback = [&reportLatch,
                               &timestamps](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    ASSERT_EQ(timestamps.size(), 0ul);
    timestamps = tonic::DartConverter<std::vector<int64_t>>::FromArguments(
        args, 0, exception);
    reportLatch.Signal();
  };
  AddNativeCallback("NativeReportTimingsCallback",
                    CREATE_NATIVE_ENTRY(nativeTimingCallback));
  RunEngine(shell.get(), std::move(configuration));

  // Pump many frames so we can trigger the report quickly instead of waiting
  // for the 1 second threshold.
  for (int i = 0; i < 200; i += 1) {
    PumpOneFrame(shell.get());
  }

  reportLatch.Wait();
  DestroyShell(std::move(shell));

  fml::TimePoint finish = fml::TimePoint::Now();
  ASSERT_TRUE(timestamps.size() > 0);
  ASSERT_TRUE(timestamps.size() % FrameTiming::kCount == 0);
  std::vector<FrameTiming> timings(timestamps.size() / FrameTiming::kCount);

  for (size_t i = 0; i * FrameTiming::kCount < timestamps.size(); i += 1) {
    for (auto phase : FrameTiming::kPhases) {
      timings[i].Set(
          phase,
          fml::TimePoint::FromEpochDelta(fml::TimeDelta::FromMicroseconds(
              timestamps[i * FrameTiming::kCount + phase])));
    }
  }
  CheckFrameTimings(timings, start, finish);
}

TEST_F(ShellTest, FrameRasterizedCallbackIsCalled) {
  fml::TimePoint start = fml::TimePoint::Now();

  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent timingLatch;
  FrameTiming timing;

  for (auto phase : FrameTiming::kPhases) {
    timing.Set(phase, fml::TimePoint());
    // Check that the time points are initially smaller than start, so
    // CheckFrameTimings will fail if they're not properly set later.
    ASSERT_TRUE(timing.Get(phase) < start);
  }

  settings.frame_rasterized_callback = [&timing,
                                        &timingLatch](const FrameTiming& t) {
    timing = t;
    timingLatch.Signal();
  };

  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("onBeginFrameMain");

  int64_t frame_target_time;
  auto nativeOnBeginFrame = [&frame_target_time](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    frame_target_time =
        tonic::DartConverter<int64_t>::FromArguments(args, 0, exception);
  };
  AddNativeCallback("NativeOnBeginFrame",
                    CREATE_NATIVE_ENTRY(nativeOnBeginFrame));

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());

  // Check that timing is properly set. This implies that
  // settings.frame_rasterized_callback is called.
  timingLatch.Wait();
  fml::TimePoint finish = fml::TimePoint::Now();
  std::vector<FrameTiming> timings = {timing};
  CheckFrameTimings(timings, start, finish);

  // Check that onBeginFrame, which is the frame_target_time, is after
  // FrameTiming's build start
  int64_t build_start =
      timing.Get(FrameTiming::kBuildStart).ToEpochDelta().ToMicroseconds();
  ASSERT_GT(frame_target_time, build_start);
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, ExternalEmbedderNoThreadMerger) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  bool end_frame_called = false;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        ASSERT_TRUE(raster_thread_merger.get() == nullptr);
        ASSERT_FALSE(should_resubmit_frame);
        end_frame_called = true;
        end_frame_latch.Signal();
      };
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kResubmitFrame, false);
  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);
  end_frame_latch.Wait();

  ASSERT_TRUE(end_frame_called);

  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_ExternalEmbedderEndFrameIsCalledWhenPostPrerollResultIsResubmit
#else
       ExternalEmbedderEndFrameIsCalledWhenPostPrerollResultIsResubmit
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  bool end_frame_called = false;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        ASSERT_TRUE(raster_thread_merger.get() != nullptr);
        ASSERT_TRUE(should_resubmit_frame);
        end_frame_called = true;
        end_frame_latch.Signal();
      };
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kResubmitFrame, true);
  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);
  end_frame_latch.Wait();

  ASSERT_TRUE(end_frame_called);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest,
#if defined(OS_FUCHSIA) || defined(WINUWP)
       // TODO(dworsham): https://github.com/flutter/flutter/issues/59816
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_OnPlatformViewDestroyDisablesThreadMerger
#else
       OnPlatformViewDestroyDisablesThreadMerger
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> thread_merger) {
        raster_thread_merger = thread_merger;
      };
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSuccess, true);

  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);

  auto result = shell->WaitForFirstFrame(fml::TimeDelta::Max());
  ASSERT_TRUE(result.ok()) << "Result: " << static_cast<int>(result.code())
                           << ": " << result.message();

  ASSERT_TRUE(raster_thread_merger->IsEnabled());

  ValidateDestroyPlatformView(shell.get());
  ASSERT_TRUE(raster_thread_merger->IsEnabled());

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());
  ASSERT_TRUE(raster_thread_merger->IsEnabled());
  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_OnPlatformViewDestroyAfterMergingThreads
#else
       OnPlatformViewDestroyAfterMergingThreads
#endif
) {
  const int ThreadMergingLease = 10;
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  std::shared_ptr<ShellTestExternalViewEmbedder> external_view_embedder;

  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (should_resubmit_frame && !raster_thread_merger->IsMerged()) {
          raster_thread_merger->MergeWithLease(ThreadMergingLease);

          ASSERT_TRUE(raster_thread_merger->IsMerged());
          external_view_embedder->UpdatePostPrerollResult(
              PostPrerollResult::kSuccess);
        }
        end_frame_latch.Signal();
      };
  external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSuccess, true);
  // Set resubmit once to trigger thread merging.
  external_view_embedder->UpdatePostPrerollResult(
      PostPrerollResult::kResubmitFrame);
  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);
  // Pump one frame to trigger thread merging.
  end_frame_latch.Wait();
  // Pump another frame to ensure threads are merged and a regular layer tree is
  // submitted.
  PumpOneFrame(shell.get(), 100, 100, builder);
  // Threads are merged here. PlatformViewNotifyDestroy should be executed
  // successfully.
  ASSERT_TRUE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));
  ValidateDestroyPlatformView(shell.get());

  // Ensure threads are unmerged after platform view destroy
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());

  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_OnPlatformViewDestroyWhenThreadsAreMerging
#else
       OnPlatformViewDestroyWhenThreadsAreMerging
#endif
) {
  const int kThreadMergingLease = 10;
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (should_resubmit_frame && !raster_thread_merger->IsMerged()) {
          raster_thread_merger->MergeWithLease(kThreadMergingLease);
        }
        end_frame_latch.Signal();
      };
  // Start with a regular layer tree with `PostPrerollResult::kSuccess` so we
  // can later check if the rasterizer is tore down using
  // |ValidateDestroyPlatformView|
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSuccess, true);

  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);
  // Pump one frame and threads aren't merged
  end_frame_latch.Wait();
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));

  // Pump a frame with `PostPrerollResult::kResubmitFrame` to start merging
  // threads
  external_view_embedder->UpdatePostPrerollResult(
      PostPrerollResult::kResubmitFrame);
  PumpOneFrame(shell.get(), 100, 100, builder);

  // Now destroy the platform view immediately.
  // Two things can happen here:
  // 1. Threads haven't merged. 2. Threads has already merged.
  // |Shell:OnPlatformViewDestroy| should be able to handle both cases.
  ValidateDestroyPlatformView(shell.get());

  // Ensure threads are unmerged after platform view destroy
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());

  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_OnPlatformViewDestroyWithThreadMergerWhileThreadsAreUnmerged
#else
       OnPlatformViewDestroyWithThreadMergerWhileThreadsAreUnmerged
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        end_frame_latch.Signal();
      };
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSuccess, true);
  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };
  PumpOneFrame(shell.get(), 100, 100, builder);
  end_frame_latch.Wait();

  // Threads should not be merged.
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));
  ValidateDestroyPlatformView(shell.get());

  // Ensure threads are unmerged after platform view destroy
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, OnPlatformViewDestroyWithoutRasterThreadMerger) {
  auto settings = CreateSettingsForFixture();

  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, nullptr);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };
  PumpOneFrame(shell.get(), 100, 100, builder);

  // Threads should not be merged.
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));
  ValidateDestroyPlatformView(shell.get());

  // Ensure threads are unmerged after platform view destroy
  ASSERT_FALSE(fml::TaskRunnerChecker::RunsOnTheSameThread(
      shell->GetTaskRunners().GetRasterTaskRunner()->GetTaskQueueId(),
      shell->GetTaskRunners().GetPlatformTaskRunner()->GetTaskQueueId()));

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());

  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_OnPlatformViewDestroyWithStaticThreadMerging
#else
       OnPlatformViewDestroyWithStaticThreadMerging
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        end_frame_latch.Signal();
      };
  auto external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSuccess, true);
  ThreadHost thread_host(
      "io.flutter.test." + GetCurrentTestName() + ".",
      ThreadHost::Type::Platform | ThreadHost::Type::IO | ThreadHost::Type::UI);
  TaskRunners task_runners(
      "test",
      thread_host.platform_thread->GetTaskRunner(),  // platform
      thread_host.platform_thread->GetTaskRunner(),  // raster
      thread_host.ui_thread->GetTaskRunner(),        // ui
      thread_host.io_thread->GetTaskRunner()         // io
  );
  auto shell = CreateShell(std::move(settings), std::move(task_runners), false,
                           external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };
  PumpOneFrame(shell.get(), 100, 100, builder);
  end_frame_latch.Wait();

  ValidateDestroyPlatformView(shell.get());

  // Validate the platform view can be recreated and destroyed again
  ValidateShell(shell.get());

  DestroyShell(std::move(shell), std::move(task_runners));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
// TODO(https://github.com/flutter/flutter/issues/66056): Deflake on all other
// platforms
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_SkipAndSubmitFrame
#else
       DISABLED_SkipAndSubmitFrame
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  std::shared_ptr<ShellTestExternalViewEmbedder> external_view_embedder;

  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (should_resubmit_frame && !raster_thread_merger->IsMerged()) {
          raster_thread_merger->MergeWithLease(10);
          external_view_embedder->UpdatePostPrerollResult(
              PostPrerollResult::kSuccess);
        }
        end_frame_latch.Signal();
      };
  external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kSkipAndRetryFrame, true);

  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);

  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));

  ASSERT_EQ(0, external_view_embedder->GetSubmittedFrameCount());

  PumpOneFrame(shell.get());

  // `EndFrame` changed the post preroll result to `kSuccess`.
  end_frame_latch.Wait();

  // Let the resubmitted frame to run and `GetSubmittedFrameCount` should be
  // called.
  end_frame_latch.Wait();
  // 2 frames are submitted because `kSkipAndRetryFrame`, but only the 2nd frame
  // should be submitted with `external_view_embedder`, hence the below check.
  ASSERT_EQ(1, external_view_embedder->GetSubmittedFrameCount());

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

// TODO(https://github.com/flutter/flutter/issues/59816): Enable on fuchsia.
TEST_F(ShellTest,
#if defined(OS_FUCHSIA)
       DISABLED_ResubmitFrame
#else
       ResubmitFrame
#endif
) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent end_frame_latch;
  std::shared_ptr<ShellTestExternalViewEmbedder> external_view_embedder;
  fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger_ref;
  auto end_frame_callback =
      [&](bool should_resubmit_frame,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (!raster_thread_merger_ref) {
          raster_thread_merger_ref = raster_thread_merger;
        }
        if (should_resubmit_frame && !raster_thread_merger->IsMerged()) {
          raster_thread_merger->MergeWithLease(10);
          external_view_embedder->UpdatePostPrerollResult(
              PostPrerollResult::kSuccess);
        }
        end_frame_latch.Signal();
      };
  external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      end_frame_callback, PostPrerollResult::kResubmitFrame, true);

  auto shell = CreateShell(std::move(settings), GetTaskRunnersForFixture(),
                           false, external_view_embedder);
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));

  ASSERT_EQ(0, external_view_embedder->GetSubmittedFrameCount());

  PumpOneFrame(shell.get());
  // `EndFrame` changed the post preroll result to `kSuccess` and merged the
  // threads. During the frame, the threads are not merged, So no
  // `external_view_embedder->GetSubmittedFrameCount()` is called.
  end_frame_latch.Wait();
  ASSERT_TRUE(raster_thread_merger_ref->IsMerged());

  // This is the resubmitted frame, which threads are also merged.
  end_frame_latch.Wait();
  // 2 frames are submitted because `kResubmitFrame`, but only the 2nd frame
  // should be submitted with `external_view_embedder`, hence the below check.
  ASSERT_EQ(1, external_view_embedder->GetSubmittedFrameCount());

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST(SettingsTest, FrameTimingSetsAndGetsProperly) {
  // Ensure that all phases are in kPhases.
  ASSERT_EQ(sizeof(FrameTiming::kPhases),
            FrameTiming::kCount * sizeof(FrameTiming::Phase));

  int lastPhaseIndex = -1;
  FrameTiming timing;
  for (auto phase : FrameTiming::kPhases) {
    ASSERT_TRUE(phase > lastPhaseIndex);  // Ensure that kPhases are in order.
    lastPhaseIndex = phase;
    auto fake_time =
        fml::TimePoint::FromEpochDelta(fml::TimeDelta::FromMicroseconds(phase));
    timing.Set(phase, fake_time);
    ASSERT_TRUE(timing.Get(phase) == fake_time);
  }
}

TEST_F(ShellTest, ReportTimingsIsCalledImmediatelyAfterTheFirstFrame) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("reportTimingsMain");
  fml::AutoResetWaitableEvent reportLatch;
  std::vector<int64_t> timestamps;
  auto nativeTimingCallback = [&reportLatch,
                               &timestamps](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    ASSERT_EQ(timestamps.size(), 0ul);
    timestamps = tonic::DartConverter<std::vector<int64_t>>::FromArguments(
        args, 0, exception);
    reportLatch.Signal();
  };
  AddNativeCallback("NativeReportTimingsCallback",
                    CREATE_NATIVE_ENTRY(nativeTimingCallback));
  ASSERT_TRUE(configuration.IsValid());
  RunEngine(shell.get(), std::move(configuration));

  for (int i = 0; i < 10; i += 1) {
    PumpOneFrame(shell.get());
  }

  reportLatch.Wait();
  DestroyShell(std::move(shell));

  // Check for the immediate callback of the first frame that doesn't wait for
  // the other 9 frames to be rasterized.
  ASSERT_EQ(timestamps.size(), FrameTiming::kCount);
}

TEST_F(ShellTest, ReloadSystemFonts) {
  auto settings = CreateSettingsForFixture();

  fml::MessageLoop::EnsureInitializedForCurrentThread();
  auto task_runner = fml::MessageLoop::GetCurrent().GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), std::move(task_runners));

  auto fontCollection = GetFontCollection(shell.get());
  std::vector<std::string> families(1, "Robotofake");
  auto font =
      fontCollection->GetMinikinFontCollectionForFamilies(families, "en");
  if (font == nullptr) {
    // The system does not have default font. Aborts this test.
    return;
  }
  unsigned int id = font->getId();
  // The result should be cached.
  font = fontCollection->GetMinikinFontCollectionForFamilies(families, "en");
  ASSERT_EQ(font->getId(), id);
  bool result = shell->ReloadSystemFonts();

  // The cache is cleared, and FontCollection will be assigned a new id.
  font = fontCollection->GetMinikinFontCollectionForFamilies(families, "en");
  ASSERT_NE(font->getId(), id);
  ASSERT_TRUE(result);
  shell.reset();
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_WaitForFirstFrame
#else
       WaitForFirstFrame
#endif
) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  fml::Status result = shell->WaitForFirstFrame(fml::TimeDelta::Max());
  ASSERT_TRUE(result.ok());

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, WaitForFirstFrameZeroSizeFrame) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get(), {1.0, 0.0, 0.0, 22});
  fml::Status result = shell->WaitForFirstFrame(fml::TimeDelta::Zero());
  ASSERT_FALSE(result.ok());
  ASSERT_EQ(result.code(), fml::StatusCode::kDeadlineExceeded);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, WaitForFirstFrameTimeout) {
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  fml::Status result = shell->WaitForFirstFrame(fml::TimeDelta::Zero());
  ASSERT_FALSE(result.ok());
  ASSERT_EQ(result.code(), fml::StatusCode::kDeadlineExceeded);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_WaitForFirstFrameMultiple
#else
       WaitForFirstFrameMultiple
#endif  // defined(WINUWP)
) {

  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  fml::Status result = shell->WaitForFirstFrame(fml::TimeDelta::Max());
  ASSERT_TRUE(result.ok());
  for (int i = 0; i < 100; ++i) {
    result = shell->WaitForFirstFrame(fml::TimeDelta::Zero());
    ASSERT_TRUE(result.ok());
  }

  DestroyShell(std::move(shell));
}

/// Makes sure that WaitForFirstFrame works if we rendered a frame with the
/// single-thread setup.
TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_WaitForFirstFrameInlined
#else
       WaitForFirstFrameInlined
#endif  // defined(WINUWP)
) {

  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  fml::AutoResetWaitableEvent event;
  task_runner->PostTask([&shell, &event] {
    fml::Status result = shell->WaitForFirstFrame(fml::TimeDelta::Max());
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.code(), fml::StatusCode::kFailedPrecondition);
    event.Signal();
  });
  ASSERT_FALSE(event.WaitWithTimeout(fml::TimeDelta::Max()));

  DestroyShell(std::move(shell), std::move(task_runners));
}

static size_t GetRasterizerResourceCacheBytesSync(const Shell& shell) {
  size_t bytes = 0;
  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell.GetTaskRunners().GetRasterTaskRunner(), [&]() {
        if (auto rasterizer = shell.GetRasterizer()) {
          bytes = rasterizer->GetResourceCacheMaxBytes().value_or(0U);
        }
        latch.Signal();
      });
  latch.Wait();
  return bytes;
}

TEST_F(ShellTest, SetResourceCacheSize) {
  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());

  // The Vulkan and GL backends set different default values for the resource
  // cache size.
#ifdef SHELL_ENABLE_VULKAN
  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell),
            vulkan::kGrCacheMaxByteSize);
#else
  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell),
            static_cast<size_t>(24 * (1 << 20)));
#endif

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [&shell]() {
        shell->GetPlatformView()->SetViewportMetrics({1.0, 400, 200, 22});
      });
  PumpOneFrame(shell.get());

  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell), 3840000U);

  std::string request_json = R"json({
                                "method": "Skia.setResourceCacheMaxBytes",
                                "args": 10000
                              })json";
  auto data =
      fml::MallocMapping::Copy(request_json.c_str(), request_json.length());
  auto platform_message = std::make_unique<PlatformMessage>(
      "flutter/skia", std::move(data), nullptr);
  SendEnginePlatformMessage(shell.get(), std::move(platform_message));
  PumpOneFrame(shell.get());
  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell), 10000U);

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [&shell]() {
        shell->GetPlatformView()->SetViewportMetrics({1.0, 800, 400, 22});
      });
  PumpOneFrame(shell.get());

  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell), 10000U);
  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest, SetResourceCacheSizeEarly) {
  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [&shell]() {
        shell->GetPlatformView()->SetViewportMetrics({1.0, 400, 200, 22});
      });
  PumpOneFrame(shell.get());

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());

  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell),
            static_cast<size_t>(3840000U));
  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest, SetResourceCacheSizeNotifiesDart) {
  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [&shell]() {
        shell->GetPlatformView()->SetViewportMetrics({1.0, 400, 200, 22});
      });
  PumpOneFrame(shell.get());

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("testSkiaResourceCacheSendsResponse");

  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell),
            static_cast<size_t>(3840000U));

  fml::AutoResetWaitableEvent latch;
  AddNativeCallback("NotifyNative", CREATE_NATIVE_ENTRY([&latch](auto args) {
                      latch.Signal();
                    }));

  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());

  latch.Wait();

  EXPECT_EQ(GetRasterizerResourceCacheBytesSync(*shell),
            static_cast<size_t>(10000U));
  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest, CanCreateImagefromDecompressedBytes) {
  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();

  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);

  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canCreateImageFromDecompressedData");

  fml::AutoResetWaitableEvent latch;
  AddNativeCallback("NotifyWidthHeight",
                    CREATE_NATIVE_ENTRY([&latch](auto args) {
                      auto width = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 0));
                      auto height = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 1));
                      ASSERT_EQ(width, 10);
                      ASSERT_EQ(height, 10);
                      latch.Signal();
                    }));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  DestroyShell(std::move(shell), std::move(task_runners));
}

class MockTexture : public Texture {
 public:
  MockTexture(int64_t textureId,
              std::shared_ptr<fml::AutoResetWaitableEvent> latch)
      : Texture(textureId), latch_(latch) {}

  ~MockTexture() override = default;

  // Called from raster thread.
  void Paint(SkCanvas& canvas,
             const SkRect& bounds,
             bool freeze,
             GrDirectContext* context,
             const SkSamplingOptions&) override {}

  void OnGrContextCreated() override {}

  void OnGrContextDestroyed() override {}

  void MarkNewFrameAvailable() override {
    frames_available_++;
    latch_->Signal();
  }

  void OnTextureUnregistered() override {
    unregistered_ = true;
    latch_->Signal();
  }

  bool unregistered() { return unregistered_; }
  int frames_available() { return frames_available_; }

 private:
  bool unregistered_ = false;
  int frames_available_ = 0;
  std::shared_ptr<fml::AutoResetWaitableEvent> latch_;
};

TEST_F(ShellTest, TextureFrameMarkedAvailableAndUnregister) {
  Settings settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  ASSERT_TRUE(ValidateShell(shell.get()));
  PlatformViewNotifyCreated(shell.get());

  RunEngine(shell.get(), std::move(configuration));

  std::shared_ptr<fml::AutoResetWaitableEvent> latch =
      std::make_shared<fml::AutoResetWaitableEvent>();

  std::shared_ptr<MockTexture> mockTexture =
      std::make_shared<MockTexture>(0, latch);

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(), [&]() {
        shell->GetPlatformView()->RegisterTexture(mockTexture);
        shell->GetPlatformView()->MarkTextureFrameAvailable(0);
      });
  latch->Wait();

  EXPECT_EQ(mockTexture->frames_available(), 1);

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(),
      [&]() { shell->GetPlatformView()->UnregisterTexture(0); });
  latch->Wait();

  EXPECT_EQ(mockTexture->unregistered(), true);
  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest, IsolateCanAccessPersistentIsolateData) {
  const std::string message = "dummy isolate launch data.";

  Settings settings = CreateSettingsForFixture();
  settings.persistent_isolate_data =
      std::make_shared<fml::DataMapping>(message);
  TaskRunners task_runners("test",                  // label
                           GetCurrentTaskRunner(),  // platform
                           CreateNewThread(),       // raster
                           CreateNewThread(),       // ui
                           CreateNewThread()        // io
  );

  fml::AutoResetWaitableEvent message_latch;
  AddNativeCallback("NotifyMessage",
                    CREATE_NATIVE_ENTRY([&](Dart_NativeArguments args) {
                      const auto message_from_dart =
                          tonic::DartConverter<std::string>::FromDart(
                              Dart_GetNativeArgument(args, 0));
                      ASSERT_EQ(message, message_from_dart);
                      message_latch.Signal();
                    }));

  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  ASSERT_TRUE(shell->IsSetup());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canAccessIsolateLaunchData");

  fml::AutoResetWaitableEvent event;
  shell->RunEngine(std::move(configuration), [&](auto result) {
    ASSERT_EQ(result, Engine::RunStatus::Success);
  });

  message_latch.Wait();
  DestroyShell(std::move(shell), std::move(task_runners));
}

static void LogSkData(sk_sp<SkData> data, const char* title) {
  FML_LOG(ERROR) << "---------- " << title;
  std::ostringstream ostr;
  for (size_t i = 0; i < data->size();) {
    ostr << std::hex << std::setfill('0') << std::setw(2)
         << static_cast<int>(data->bytes()[i]) << " ";
    i++;
    if (i % 16 == 0 || i == data->size()) {
      FML_LOG(ERROR) << ostr.str();
      ostr.str("");
      ostr.clear();
    }
  }
}

TEST_F(ShellTest, Screenshot) {
  auto settings = CreateSettingsForFixture();
  fml::AutoResetWaitableEvent firstFrameLatch;
  settings.frame_rasterized_callback =
      [&firstFrameLatch](const FrameTiming& t) { firstFrameLatch.Signal(); };

  std::unique_ptr<Shell> shell = CreateShell(settings);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  LayerTreeBuilder builder = [&](std::shared_ptr<ContainerLayer> root) {
    SkPictureRecorder recorder;
    SkCanvas* recording_canvas =
        recorder.beginRecording(SkRect::MakeXYWH(0, 0, 80, 80));
    recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, 80, 80),
                               SkPaint(SkColor4f::FromColor(SK_ColorRED)));
    auto sk_picture = recorder.finishRecordingAsPicture();
    fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
        this->GetCurrentTaskRunner(), fml::TimeDelta::Zero());
    auto picture_layer = std::make_shared<PictureLayer>(
        SkPoint::Make(10, 10),
        flutter::SkiaGPUObject<SkPicture>({sk_picture, queue}), false, false);
    root->Add(picture_layer);
  };

  PumpOneFrame(shell.get(), 100, 100, builder);
  firstFrameLatch.Wait();

  std::promise<Rasterizer::Screenshot> screenshot_promise;
  auto screenshot_future = screenshot_promise.get_future();

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(),
      [&screenshot_promise, &shell]() {
        auto rasterizer = shell->GetRasterizer();
        screenshot_promise.set_value(rasterizer->ScreenshotLastLayerTree(
            Rasterizer::ScreenshotType::CompressedImage, false));
      });

  auto fixtures_dir =
      fml::OpenDirectory(GetFixturesPath(), false, fml::FilePermission::kRead);

  auto reference_png = fml::FileMapping::CreateReadOnly(
      fixtures_dir, "shelltest_screenshot.png");

  // Use MakeWithoutCopy instead of MakeWithCString because we don't want to
  // encode the null sentinel
  sk_sp<SkData> reference_data = SkData::MakeWithoutCopy(
      reference_png->GetMapping(), reference_png->GetSize());

  sk_sp<SkData> screenshot_data = screenshot_future.get().data;
  if (!reference_data->equals(screenshot_data.get())) {
    LogSkData(reference_data, "reference");
    LogSkData(screenshot_data, "screenshot");
    ASSERT_TRUE(false);
  }

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, CanConvertToAndFromMappings) {
  const size_t buffer_size = 2 << 20;

  uint8_t* buffer = static_cast<uint8_t*>(::malloc(buffer_size));
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(MemsetPatternSetOrCheck(
      buffer, buffer_size, MemsetPatternOp::kMemsetPatternOpSetBuffer));

  std::unique_ptr<fml::Mapping> mapping =
      std::make_unique<fml::MallocMapping>(buffer, buffer_size);

  ASSERT_EQ(mapping->GetSize(), buffer_size);

  fml::AutoResetWaitableEvent latch;
  AddNativeCallback(
      "SendFixtureMapping", CREATE_NATIVE_ENTRY([&](auto args) {
        auto mapping_from_dart =
            tonic::DartConverter<std::unique_ptr<fml::Mapping>>::FromDart(
                Dart_GetNativeArgument(args, 0));
        ASSERT_NE(mapping_from_dart, nullptr);
        ASSERT_EQ(mapping_from_dart->GetSize(), buffer_size);
        ASSERT_TRUE(MemsetPatternSetOrCheck(
            const_cast<uint8_t*>(mapping_from_dart->GetMapping()),  // buffer
            mapping_from_dart->GetSize(),                           // size
            MemsetPatternOp::kMemsetPatternOpCheckBuffer            // op
            ));
        latch.Signal();
      }));

  AddNativeCallback(
      "GetFixtureMapping", CREATE_NATIVE_ENTRY([&](auto args) {
        tonic::DartConverter<tonic::DartConverterMapping>::SetReturnValue(
            args, mapping);
      }));

  auto settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canConvertMappings");
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ASSERT_NE(shell.get(), nullptr);
  RunEngine(shell.get(), std::move(configuration));
  latch.Wait();
  DestroyShell(std::move(shell));
}

// Compares local times as seen by the dart isolate and as seen by this test
// fixture, to a resolution of 1 hour.
//
// This verifies that (1) the isolate is able to get a timezone (doesn't lock
// up for example), and (2) that the host and the isolate agree on what the
// timezone is.
TEST_F(ShellTest, LocaltimesMatch) {
  fml::AutoResetWaitableEvent latch;
  std::string dart_isolate_time_str;

  // See fixtures/shell_test.dart, the callback NotifyLocalTime is declared
  // there.
  AddNativeCallback("NotifyLocalTime", CREATE_NATIVE_ENTRY([&](auto args) {
                      dart_isolate_time_str =
                          tonic::DartConverter<std::string>::FromDart(
                              Dart_GetNativeArgument(args, 0));
                      latch.Signal();
                    }));

  auto settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("localtimesMatch");
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ASSERT_NE(shell.get(), nullptr);
  RunEngine(shell.get(), std::move(configuration));
  latch.Wait();

  char timestr[200];
  const time_t timestamp = time(nullptr);
  const struct tm* local_time = localtime(&timestamp);
  ASSERT_NE(local_time, nullptr)
      << "Could not get local time: errno=" << errno << ": " << strerror(errno);
  // Example: "2020-02-26 14" for 2pm on February 26, 2020.
  const size_t format_size =
      strftime(timestr, sizeof(timestr), "%Y-%m-%d %H", local_time);
  ASSERT_NE(format_size, 0UL)
      << "strftime failed: host time: " << std::string(timestr)
      << " dart isolate time: " << dart_isolate_time_str;

  const std::string host_local_time_str = timestr;

  ASSERT_EQ(dart_isolate_time_str, host_local_time_str)
      << "Local times in the dart isolate and the local time seen by the test "
      << "differ by more than 1 hour, but are expected to be about equal";

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, CanDecompressImageFromAsset) {
  fml::AutoResetWaitableEvent latch;
  AddNativeCallback("NotifyWidthHeight", CREATE_NATIVE_ENTRY([&](auto args) {
                      auto width = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 0));
                      auto height = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 1));
                      ASSERT_EQ(width, 100);
                      ASSERT_EQ(height, 100);
                      latch.Signal();
                    }));

  AddNativeCallback(
      "GetFixtureImage", CREATE_NATIVE_ENTRY([](auto args) {
        auto fixture = OpenFixtureAsMapping("shelltest_screenshot.png");
        tonic::DartConverter<tonic::DartConverterMapping>::SetReturnValue(
            args, fixture);
      }));

  auto settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canDecompressImageFromAsset");
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ASSERT_NE(shell.get(), nullptr);
  RunEngine(shell.get(), std::move(configuration));
  latch.Wait();
  DestroyShell(std::move(shell));
}

/// An image generator that always creates a 1x1 single-frame green image.
class SinglePixelImageGenerator : public ImageGenerator {
 public:
  SinglePixelImageGenerator()
      : info_(SkImageInfo::MakeN32(1, 1, SkAlphaType::kOpaque_SkAlphaType)){};
  ~SinglePixelImageGenerator() = default;
  const SkImageInfo& GetInfo() { return info_; }

  unsigned int GetFrameCount() const { return 1; }

  unsigned int GetPlayCount() const { return 1; }

  const ImageGenerator::FrameInfo GetFrameInfo(unsigned int frame_index) const {
    return {std::nullopt, 0, SkCodecAnimation::DisposalMethod::kKeep};
  }

  SkISize GetScaledDimensions(float scale) {
    return SkISize::Make(info_.width(), info_.height());
  }

  bool GetPixels(const SkImageInfo& info,
                 void* pixels,
                 size_t row_bytes,
                 unsigned int frame_index,
                 std::optional<unsigned int> prior_frame) {
    assert(info.width() == 1);
    assert(info.height() == 1);
    assert(row_bytes == 4);

    reinterpret_cast<uint32_t*>(pixels)[0] = 0x00ff00ff;
    return true;
  };

 private:
  SkImageInfo info_;
};

TEST_F(ShellTest, CanRegisterImageDecoders) {
  fml::AutoResetWaitableEvent latch;
  AddNativeCallback("NotifyWidthHeight", CREATE_NATIVE_ENTRY([&](auto args) {
                      auto width = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 0));
                      auto height = tonic::DartConverter<int>::FromDart(
                          Dart_GetNativeArgument(args, 1));
                      ASSERT_EQ(width, 1);
                      ASSERT_EQ(height, 1);
                      latch.Signal();
                    }));

  auto settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canRegisterImageDecoders");
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ASSERT_NE(shell.get(), nullptr);

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(), [&shell]() {
        shell->RegisterImageDecoder(
            [](sk_sp<SkData> buffer) {
              return std::make_unique<SinglePixelImageGenerator>();
            },
            100);
      });

  RunEngine(shell.get(), std::move(configuration));
  latch.Wait();
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_OnServiceProtocolGetSkSLsWorks
#else
       OnServiceProtocolGetSkSLsWorks
#endif  // defined(WINUWP)
) {
  fml::ScopedTemporaryDirectory base_dir;
  ASSERT_TRUE(base_dir.fd().is_valid());
  PersistentCache::SetCacheDirectoryPath(base_dir.path());
  PersistentCache::ResetCacheForProcess();

  // Create 2 dummy SkSL cache file IE (base32 encoding of A), II (base32
  // encoding of B) with content x and y.
  std::vector<std::string> components = {
      "flutter_engine", GetFlutterEngineVersion(), "skia", GetSkiaVersion(),
      PersistentCache::kSkSLSubdirName};
  auto sksl_dir = fml::CreateDirectory(base_dir.fd(), components,
                                       fml::FilePermission::kReadWrite);
  const std::string x_key_str = "A";
  const std::string x_value_str = "x";
  sk_sp<SkData> x_key =
      SkData::MakeWithCopy(x_key_str.data(), x_key_str.size());
  sk_sp<SkData> x_value =
      SkData::MakeWithCopy(x_value_str.data(), x_value_str.size());
  auto x_data = PersistentCache::BuildCacheObject(*x_key, *x_value);

  const std::string y_key_str = "B";
  const std::string y_value_str = "y";
  sk_sp<SkData> y_key =
      SkData::MakeWithCopy(y_key_str.data(), y_key_str.size());
  sk_sp<SkData> y_value =
      SkData::MakeWithCopy(y_value_str.data(), y_value_str.size());
  auto y_data = PersistentCache::BuildCacheObject(*y_key, *y_value);

  ASSERT_TRUE(fml::WriteAtomically(sksl_dir, "x_cache", *x_data));
  ASSERT_TRUE(fml::WriteAtomically(sksl_dir, "y_cache", *y_data));

  Settings settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);
  ServiceProtocol::Handler::ServiceProtocolMap empty_params;
  rapidjson::Document document;
  OnServiceProtocol(shell.get(), ServiceProtocolEnum::kGetSkSLs,
                    shell->GetTaskRunners().GetIOTaskRunner(), empty_params,
                    &document);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  DestroyShell(std::move(shell));

  const std::string expected_json1 =
      "{\"type\":\"GetSkSLs\",\"SkSLs\":{\"II\":\"eQ==\",\"IE\":\"eA==\"}}";
  const std::string expected_json2 =
      "{\"type\":\"GetSkSLs\",\"SkSLs\":{\"IE\":\"eA==\",\"II\":\"eQ==\"}}";
  bool json_is_expected = (expected_json1 == buffer.GetString()) ||
                          (expected_json2 == buffer.GetString());
  ASSERT_TRUE(json_is_expected) << buffer.GetString() << " is not equal to "
                                << expected_json1 << " or " << expected_json2;
}

TEST_F(ShellTest, RasterizerScreenshot) {
  Settings settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  ASSERT_TRUE(ValidateShell(shell.get()));
  PlatformViewNotifyCreated(shell.get());

  RunEngine(shell.get(), std::move(configuration));

  auto latch = std::make_shared<fml::AutoResetWaitableEvent>();

  PumpOneFrame(shell.get());

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(), [&shell, &latch]() {
        Rasterizer::Screenshot screenshot =
            shell->GetRasterizer()->ScreenshotLastLayerTree(
                Rasterizer::ScreenshotType::CompressedImage, true);
        EXPECT_NE(screenshot.data, nullptr);

        latch->Signal();
      });
  latch->Wait();
  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest, RasterizerMakeRasterSnapshot) {
  Settings settings = CreateSettingsForFixture();
  auto configuration = RunConfiguration::InferFromSettings(settings);
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  ASSERT_TRUE(ValidateShell(shell.get()));
  PlatformViewNotifyCreated(shell.get());

  RunEngine(shell.get(), std::move(configuration));

  auto latch = std::make_shared<fml::AutoResetWaitableEvent>();

  PumpOneFrame(shell.get());

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetRasterTaskRunner(), [&shell, &latch]() {
        SnapshotDelegate* delegate =
            reinterpret_cast<Rasterizer*>(shell->GetRasterizer().get());
        sk_sp<SkImage> image = delegate->MakeRasterSnapshot(
            SkPicture::MakePlaceholder({0, 0, 50, 50}), SkISize::Make(50, 50));
        EXPECT_NE(image, nullptr);

        latch->Signal();
      });
  latch->Wait();
  DestroyShell(std::move(shell), std::move(task_runners));
}

static sk_sp<SkPicture> MakeSizedPicture(int width, int height) {
  SkPictureRecorder recorder;
  SkCanvas* recording_canvas =
      recorder.beginRecording(SkRect::MakeXYWH(0, 0, width, height));
  recording_canvas->drawRect(SkRect::MakeXYWH(0, 0, width, height),
                             SkPaint(SkColor4f::FromColor(SK_ColorRED)));
  return recorder.finishRecordingAsPicture();
}

TEST_F(ShellTest, OnServiceProtocolEstimateRasterCacheMemoryWorks) {
  Settings settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);

  // 1. Construct a picture and a picture layer to be raster cached.
  sk_sp<SkPicture> picture = MakeSizedPicture(10, 10);
  fml::RefPtr<SkiaUnrefQueue> queue = fml::MakeRefCounted<SkiaUnrefQueue>(
      GetCurrentTaskRunner(), fml::TimeDelta::Zero());
  auto picture_layer = std::make_shared<PictureLayer>(
      SkPoint::Make(0, 0),
      flutter::SkiaGPUObject<SkPicture>({MakeSizedPicture(100, 100), queue}),
      false, false);
  picture_layer->set_paint_bounds(SkRect::MakeWH(100, 100));

  // 2. Rasterize the picture and the picture layer in the raster cache.
  std::promise<bool> rasterized;
  shell->GetTaskRunners().GetRasterTaskRunner()->PostTask(
      [&shell, &rasterized, &picture, &picture_layer] {
        auto* compositor_context = shell->GetRasterizer()->compositor_context();
        auto& raster_cache = compositor_context->raster_cache();

        Stopwatch raster_time;
        Stopwatch ui_time;
        MutatorsStack mutators_stack;
        TextureRegistry texture_registry;
        PrerollContext preroll_context = {
            nullptr,                 /* raster_cache */
            nullptr,                 /* gr_context */
            nullptr,                 /* external_view_embedder */
            mutators_stack, nullptr, /* color_space */
            kGiantRect,              /* cull_rect */
            false,                   /* layer reads from surface */
            raster_time,    ui_time, texture_registry,
            false, /* checkerboard_offscreen_layers */
            1.0f,  /* frame_device_pixel_ratio */
            false, /* has_platform_view */
        };

        // 2.1. Rasterize the picture. Call Draw multiple times to pass the
        // access threshold (default to 3) so a cache can be generated.
        SkCanvas dummy_canvas;
        bool picture_cache_generated;
        for (int i = 0; i < 4; i += 1) {
          SkMatrix matrix = SkMatrix::I();

          picture_cache_generated = raster_cache.Prepare(
              &preroll_context, picture.get(), true, false, matrix);
          raster_cache.Draw(*picture, dummy_canvas);
        }
        ASSERT_TRUE(picture_cache_generated);

        // 2.2. Rasterize the picture layer.
        raster_cache.Prepare(&preroll_context, picture_layer.get(),
                             SkMatrix::I());
        rasterized.set_value(true);
      });
  rasterized.get_future().wait();

  // 3. Call the service protocol and check its output.
  ServiceProtocol::Handler::ServiceProtocolMap empty_params;
  rapidjson::Document document;
  OnServiceProtocol(
      shell.get(), ServiceProtocolEnum::kEstimateRasterCacheMemory,
      shell->GetTaskRunners().GetRasterTaskRunner(), empty_params, &document);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  std::string expected_json =
      "{\"type\":\"EstimateRasterCacheMemory\",\"layerBytes\":40000,\"picture"
      "Bytes\":400}";
  std::string actual_json = buffer.GetString();
  ASSERT_EQ(actual_json, expected_json);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, DiscardLayerTreeOnResize) {
  auto settings = CreateSettingsForFixture();

  SkISize wrong_size = SkISize::Make(400, 100);
  SkISize expected_size = SkISize::Make(400, 200);

  fml::AutoResetWaitableEvent end_frame_latch;
  std::shared_ptr<ShellTestExternalViewEmbedder> external_view_embedder;
  fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger_ref;
  auto end_frame_callback =
      [&](bool should_merge_thread,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (!raster_thread_merger_ref) {
          raster_thread_merger_ref = raster_thread_merger;
        }
        if (should_merge_thread) {
          // TODO(cyanglaz): This test used external_view_embedder so we need to
          // merge the threads here. However, the scenario it is testing is
          // unrelated to platform views. We should consider to update this test
          // so it doesn't require external_view_embedder.
          // https://github.com/flutter/flutter/issues/69895
          raster_thread_merger->MergeWithLease(10);
          external_view_embedder->UpdatePostPrerollResult(
              PostPrerollResult::kSuccess);
        }
        end_frame_latch.Signal();
      };

  external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      std::move(end_frame_callback), PostPrerollResult::kResubmitFrame, true);

  std::unique_ptr<Shell> shell = CreateShell(
      settings, GetTaskRunnersForFixture(), false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [&shell, &expected_size]() {
        shell->GetPlatformView()->SetViewportMetrics(
            {1.0, static_cast<double>(expected_size.width()),
             static_cast<double>(expected_size.height()), 22});
      });

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  PumpOneFrame(shell.get(), static_cast<double>(wrong_size.width()),
               static_cast<double>(wrong_size.height()));

  end_frame_latch.Wait();

  ASSERT_EQ(0, external_view_embedder->GetSubmittedFrameCount());

  // Threads will be merged at the end of this frame.
  PumpOneFrame(shell.get(), static_cast<double>(expected_size.width()),
               static_cast<double>(expected_size.height()));

  end_frame_latch.Wait();
  ASSERT_TRUE(raster_thread_merger_ref->IsMerged());

  end_frame_latch.Wait();
  // 2 frames are submitted because `kResubmitFrame`, but only the 2nd frame
  // should be submitted with `external_view_embedder`, hence the below check.
  ASSERT_EQ(1, external_view_embedder->GetSubmittedFrameCount());
  ASSERT_EQ(expected_size, external_view_embedder->GetLastSubmittedFrameSize());

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, DiscardResubmittedLayerTreeOnResize) {
  auto settings = CreateSettingsForFixture();

  SkISize origin_size = SkISize::Make(400, 100);
  SkISize new_size = SkISize::Make(400, 200);

  fml::AutoResetWaitableEvent end_frame_latch;

  fml::AutoResetWaitableEvent resize_latch;

  std::shared_ptr<ShellTestExternalViewEmbedder> external_view_embedder;
  fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger_ref;
  auto end_frame_callback =
      [&](bool should_merge_thread,
          fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger) {
        if (!raster_thread_merger_ref) {
          raster_thread_merger_ref = raster_thread_merger;
        }
        if (should_merge_thread) {
          raster_thread_merger->MergeWithLease(10);
          external_view_embedder->UpdatePostPrerollResult(
              PostPrerollResult::kSuccess);
        }
        end_frame_latch.Signal();

        if (should_merge_thread) {
          resize_latch.Wait();
        }
      };

  external_view_embedder = std::make_shared<ShellTestExternalViewEmbedder>(
      std::move(end_frame_callback), PostPrerollResult::kResubmitFrame, true);

  std::unique_ptr<Shell> shell = CreateShell(
      settings, GetTaskRunnersForFixture(), false, external_view_embedder);

  // Create the surface needed by rasterizer
  PlatformViewNotifyCreated(shell.get());

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [&shell, &origin_size]() {
        shell->GetPlatformView()->SetViewportMetrics(
            {1.0, static_cast<double>(origin_size.width()),
             static_cast<double>(origin_size.height()), 22});
      });

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");

  RunEngine(shell.get(), std::move(configuration));

  PumpOneFrame(shell.get(), static_cast<double>(origin_size.width()),
               static_cast<double>(origin_size.height()));

  end_frame_latch.Wait();
  ASSERT_EQ(0, external_view_embedder->GetSubmittedFrameCount());

  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [&shell, &new_size, &resize_latch]() {
        shell->GetPlatformView()->SetViewportMetrics(
            {1.0, static_cast<double>(new_size.width()),
             static_cast<double>(new_size.height()), 22});
        resize_latch.Signal();
      });

  end_frame_latch.Wait();

  // The frame resubmitted with origin size should be discarded after the
  // viewport metrics changed.
  ASSERT_EQ(0, external_view_embedder->GetSubmittedFrameCount());

  // Threads will be merged at the end of this frame.
  PumpOneFrame(shell.get(), static_cast<double>(new_size.width()),
               static_cast<double>(new_size.height()));

  end_frame_latch.Wait();
  ASSERT_TRUE(raster_thread_merger_ref->IsMerged());
  ASSERT_EQ(1, external_view_embedder->GetSubmittedFrameCount());
  ASSERT_EQ(new_size, external_view_embedder->GetLastSubmittedFrameSize());

  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, IgnoresInvalidMetrics) {
  fml::AutoResetWaitableEvent latch;
  double last_device_pixel_ratio;
  double last_width;
  double last_height;
  auto native_report_device_pixel_ratio = [&](Dart_NativeArguments args) {
    auto dpr_handle = Dart_GetNativeArgument(args, 0);
    ASSERT_TRUE(Dart_IsDouble(dpr_handle));
    Dart_DoubleValue(dpr_handle, &last_device_pixel_ratio);
    ASSERT_FALSE(last_device_pixel_ratio == 0.0);

    auto width_handle = Dart_GetNativeArgument(args, 1);
    ASSERT_TRUE(Dart_IsDouble(width_handle));
    Dart_DoubleValue(width_handle, &last_width);
    ASSERT_FALSE(last_width == 0.0);

    auto height_handle = Dart_GetNativeArgument(args, 2);
    ASSERT_TRUE(Dart_IsDouble(height_handle));
    Dart_DoubleValue(height_handle, &last_height);
    ASSERT_FALSE(last_height == 0.0);

    latch.Signal();
  };

  Settings settings = CreateSettingsForFixture();
  auto task_runner = CreateNewThread();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);

  AddNativeCallback("ReportMetrics",
                    CREATE_NATIVE_ENTRY(native_report_device_pixel_ratio));

  std::unique_ptr<Shell> shell =
      CreateShell(std::move(settings), std::move(task_runners));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("reportMetrics");

  RunEngine(shell.get(), std::move(configuration));

  task_runner->PostTask([&]() {
    shell->GetPlatformView()->SetViewportMetrics({0.0, 400, 200, 22});
    task_runner->PostTask([&]() {
      shell->GetPlatformView()->SetViewportMetrics({0.8, 0.0, 200, 22});
      task_runner->PostTask([&]() {
        shell->GetPlatformView()->SetViewportMetrics({0.8, 400, 0.0, 22});
        task_runner->PostTask([&]() {
          shell->GetPlatformView()->SetViewportMetrics({0.8, 400, 200.0, 22});
        });
      });
    });
  });
  latch.Wait();
  ASSERT_EQ(last_device_pixel_ratio, 0.8);
  ASSERT_EQ(last_width, 400.0);
  ASSERT_EQ(last_height, 200.0);
  latch.Reset();

  task_runner->PostTask([&]() {
    shell->GetPlatformView()->SetViewportMetrics({1.2, 600, 300, 22});
  });
  latch.Wait();
  ASSERT_EQ(last_device_pixel_ratio, 1.2);
  ASSERT_EQ(last_width, 600.0);
  ASSERT_EQ(last_height, 300.0);

  DestroyShell(std::move(shell), std::move(task_runners));
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_OnServiceProtocolSetAssetBundlePathWorks
#else
       OnServiceProtocolSetAssetBundlePathWorks
#endif  // defined(WINUWP)
) {

  Settings settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell = CreateShell(settings);
  RunConfiguration configuration =
      RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("canAccessResourceFromAssetDir");

  // Verify isolate can load a known resource with the
  // default asset directory - kernel_blob.bin
  fml::AutoResetWaitableEvent latch;

  // Callback used to signal whether the resource was loaded successfully.
  bool can_access_resource = false;
  auto native_can_access_resource = [&can_access_resource,
                                     &latch](Dart_NativeArguments args) {
    Dart_Handle exception = nullptr;
    can_access_resource =
        tonic::DartConverter<bool>::FromArguments(args, 0, exception);
    latch.Signal();
  };
  AddNativeCallback("NotifyCanAccessResource",
                    CREATE_NATIVE_ENTRY(native_can_access_resource));

  // Callback used to delay the asset load until after the service
  // protocol method has finished.
  auto native_notify_set_asset_bundle_path =
      [&shell](Dart_NativeArguments args) {
        // Update the asset directory to a bonus path.
        ServiceProtocol::Handler::ServiceProtocolMap params;
        params["assetDirectory"] = "assetDirectory";
        rapidjson::Document document;
        OnServiceProtocol(shell.get(), ServiceProtocolEnum::kSetAssetBundlePath,
                          shell->GetTaskRunners().GetUITaskRunner(), params,
                          &document);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
      };
  AddNativeCallback("NotifySetAssetBundlePath",
                    CREATE_NATIVE_ENTRY(native_notify_set_asset_bundle_path));

  RunEngine(shell.get(), std::move(configuration));

  latch.Wait();
  ASSERT_TRUE(can_access_resource);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, EngineRootIsolateLaunchesDontTakeVMDataSettings) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  // Make sure the shell launch does not kick off the creation of the VM
  // instance by already creating one upfront.
  auto vm_settings = CreateSettingsForFixture();
  auto vm_ref = DartVMRef::Create(vm_settings);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  auto settings = vm_settings;
  fml::AutoResetWaitableEvent isolate_create_latch;
  settings.root_isolate_create_callback = [&](const auto& isolate) {
    isolate_create_latch.Signal();
  };
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));
  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  RunEngine(shell.get(), std::move(configuration));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  DestroyShell(std::move(shell));
  isolate_create_latch.Wait();
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_AssetManagerSingle
#else
       AssetManagerSingle
#endif  // defined(WINUWP)
) {

  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);

  std::string filename = "test_name";
  std::string content = "test_content";

  bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                      fml::DataMapping(content));
  ASSERT_TRUE(success);

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mapping = asset_manager.GetAsMapping(filename);
  ASSERT_TRUE(mapping != nullptr);

  std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                     mapping->GetSize());

  ASSERT_TRUE(result == content);
}

TEST_F(ShellTest,
#if defined(WINUWP)
       // TODO(cbracken): https://github.com/flutter/flutter/issues/90481
       DISABLED_AssetManagerMulti
#else
       AssetManagerMulti
#endif  // defined(WINUWP)
) {

  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);

  std::vector<std::string> filenames = {
      "good0",
      "bad0",
      "good1",
      "bad1",
  };

  for (auto filename : filenames) {
    bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mappings = asset_manager.GetAsMappings("(.*)", std::nullopt);
  EXPECT_EQ(mappings.size(), 4u);

  std::vector<std::string> expected_results = {
      "good0",
      "good1",
  };

  mappings = asset_manager.GetAsMappings("(.*)good(.*)", std::nullopt);
  ASSERT_EQ(mappings.size(), expected_results.size());

  for (auto& mapping : mappings) {
    std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                       mapping->GetSize());
    EXPECT_NE(
        std::find(expected_results.begin(), expected_results.end(), result),
        expected_results.end());
  }
}

#if defined(OS_FUCHSIA)
TEST_F(ShellTest, AssetManagerMultiSubdir) {
  std::string subdir_path = "subdir";

  fml::ScopedTemporaryDirectory asset_dir;
  fml::UniqueFD asset_dir_fd = fml::OpenDirectory(
      asset_dir.path().c_str(), false, fml::FilePermission::kRead);
  fml::UniqueFD subdir_fd =
      fml::OpenDirectory((asset_dir.path() + "/" + subdir_path).c_str(), true,
                         fml::FilePermission::kReadWrite);

  std::vector<std::string> filenames = {
      "bad0",
      "notgood",  // this is to make sure the pattern (.*)good(.*) only matches
                  // things in the subdirectory
  };

  std::vector<std::string> subdir_filenames = {
      "good0",
      "good1",
      "bad1",
  };

  for (auto filename : filenames) {
    bool success = fml::WriteAtomically(asset_dir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  for (auto filename : subdir_filenames) {
    bool success = fml::WriteAtomically(subdir_fd, filename.c_str(),
                                        fml::DataMapping(filename));
    ASSERT_TRUE(success);
  }

  AssetManager asset_manager;
  asset_manager.PushBack(
      std::make_unique<DirectoryAssetBundle>(std::move(asset_dir_fd), false));

  auto mappings = asset_manager.GetAsMappings("(.*)", std::nullopt);
  EXPECT_EQ(mappings.size(), 5u);

  mappings = asset_manager.GetAsMappings("(.*)", subdir_path);
  EXPECT_EQ(mappings.size(), 3u);

  std::vector<std::string> expected_results = {
      "good0",
      "good1",
  };

  mappings = asset_manager.GetAsMappings("(.*)good(.*)", subdir_path);
  ASSERT_EQ(mappings.size(), expected_results.size());

  for (auto& mapping : mappings) {
    std::string result(reinterpret_cast<const char*>(mapping->GetMapping()),
                       mapping->GetSize());
    ASSERT_NE(
        std::find(expected_results.begin(), expected_results.end(), result),
        expected_results.end());
  }
}
#endif  // OS_FUCHSIA

TEST_F(ShellTest, Spawn) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());
  configuration.SetEntrypoint("fixturesAreFunctionalMain");

  auto second_configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(second_configuration.IsValid());
  second_configuration.SetEntrypoint("testCanLaunchSecondaryIsolate");

  const std::string initial_route("/foo");

  fml::AutoResetWaitableEvent main_latch;
  std::string last_entry_point;
  // Fulfill native function for the first Shell's entrypoint.
  AddNativeCallback(
      "SayHiFromFixturesAreFunctionalMain", CREATE_NATIVE_ENTRY([&](auto args) {
        last_entry_point = shell->GetEngine()->GetLastEntrypoint();
        main_latch.Signal();
      }));
  // Fulfill native function for the second Shell's entrypoint.
  fml::CountDownLatch second_latch(2);
  AddNativeCallback(
      // The Dart native function names aren't very consistent but this is just
      // the native function name of the second vm entrypoint in the fixture.
      "NotifyNative",
      CREATE_NATIVE_ENTRY([&](auto args) { second_latch.CountDown(); }));

  RunEngine(shell.get(), std::move(configuration));
  main_latch.Wait();
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  // Check first Shell ran the first entrypoint.
  ASSERT_EQ("fixturesAreFunctionalMain", last_entry_point);

  PostSync(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [this, &spawner = shell, &second_configuration, &second_latch,
       initial_route]() {
        MockPlatformViewDelegate platform_view_delegate;
        auto spawn = spawner->Spawn(
            std::move(second_configuration), initial_route,
            [&platform_view_delegate](Shell& shell) {
              auto result = std::make_unique<MockPlatformView>(
                  platform_view_delegate, shell.GetTaskRunners());
              ON_CALL(*result, CreateRenderingSurface())
                  .WillByDefault(::testing::Invoke(
                      [] { return std::make_unique<MockSurface>(); }));
              return result;
            },
            [](Shell& shell) { return std::make_unique<Rasterizer>(shell); });
        ASSERT_NE(nullptr, spawn.get());
        ASSERT_TRUE(ValidateShell(spawn.get()));

        PostSync(spawner->GetTaskRunners().GetUITaskRunner(),
                 [&spawn, &spawner, initial_route] {
                   // Check second shell ran the second entrypoint.
                   ASSERT_EQ("testCanLaunchSecondaryIsolate",
                             spawn->GetEngine()->GetLastEntrypoint());
                   ASSERT_EQ(initial_route, spawn->GetEngine()->InitialRoute());

                   // TODO(74520): Remove conditional once isolate groups are
                   // supported by JIT.
                   if (DartVM::IsRunningPrecompiledCode()) {
                     ASSERT_NE(spawner->GetEngine()
                                   ->GetRuntimeController()
                                   ->GetRootIsolateGroup(),
                               0u);
                     ASSERT_EQ(spawner->GetEngine()
                                   ->GetRuntimeController()
                                   ->GetRootIsolateGroup(),
                               spawn->GetEngine()
                                   ->GetRuntimeController()
                                   ->GetRootIsolateGroup());
                   }
                 });

        PostSync(
            spawner->GetTaskRunners().GetIOTaskRunner(), [&spawner, &spawn] {
              ASSERT_EQ(spawner->GetIOManager()->GetResourceContext().get(),
                        spawn->GetIOManager()->GetResourceContext().get());
            });

        // Before destroying the shell, wait for expectations of the spawned
        // isolate to be met.
        second_latch.Wait();

        DestroyShell(std::move(spawn));
      });

  DestroyShell(std::move(shell));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeReplaces) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();
  RunEngine(shell.get(), std::move(configuration));

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), std::move(task_runners));

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeAppends) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();
  RunEngine(shell.get(), std::move(configuration));

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), std::move(task_runners));

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeNull) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();
  RunEngine(shell.get(), std::move(configuration));

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), std::move(task_runners));

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  platform_view->UpdateAssetResolverByType(
      std::move(nullptr), AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 2ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());
  ASSERT_TRUE(resolvers[1]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, UpdateAssetResolverByTypeDoesNotReplaceMismatchType) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  Settings settings = CreateSettingsForFixture();
  ThreadHost thread_host("io.flutter.test." + GetCurrentTestName() + ".",
                         ThreadHost::Type::Platform);
  auto task_runner = thread_host.platform_thread->GetTaskRunner();
  TaskRunners task_runners("test", task_runner, task_runner, task_runner,
                           task_runner);
  auto shell = CreateShell(std::move(settings), task_runners);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  auto asset_manager = configuration.GetAssetManager();
  RunEngine(shell.get(), std::move(configuration));

  auto platform_view =
      std::make_unique<PlatformView>(*shell.get(), std::move(task_runners));

  auto old_resolver = std::make_unique<TestAssetResolver>(
      true, AssetResolver::AssetResolverType::kAssetManager);
  ASSERT_TRUE(old_resolver->IsValid());
  asset_manager->PushBack(std::move(old_resolver));

  auto updated_resolver = std::make_unique<TestAssetResolver>(
      false, AssetResolver::AssetResolverType::kApkAssetProvider);
  ASSERT_FALSE(updated_resolver->IsValidAfterAssetManagerChange());
  platform_view->UpdateAssetResolverByType(
      std::move(updated_resolver),
      AssetResolver::AssetResolverType::kApkAssetProvider);

  auto resolvers = asset_manager->TakeResolvers();
  ASSERT_EQ(resolvers.size(), 3ull);
  ASSERT_TRUE(resolvers[0]->IsValidAfterAssetManagerChange());

  ASSERT_TRUE(resolvers[1]->IsValidAfterAssetManagerChange());

  ASSERT_FALSE(resolvers[2]->IsValidAfterAssetManagerChange());

  DestroyShell(std::move(shell), std::move(task_runners));
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
}

TEST_F(ShellTest, CanCreateShellsWithGLBackend) {
#if !SHELL_ENABLE_GL
  // GL emulation does not exist on Fuchsia.
  GTEST_SKIP();
#endif  // !SHELL_ENABLE_GL
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell =
      CreateShell(settings,                                       //
                  GetTaskRunnersForFixture(),                     //
                  false,                                          //
                  nullptr,                                        //
                  false,                                          //
                  ShellTestPlatformView::BackendType::kGLBackend  //
      );
  ASSERT_NE(shell, nullptr);
  ASSERT_TRUE(shell->IsSetup());
  PlatformViewNotifyCreated(shell.get());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, CanCreateShellsWithVulkanBackend) {
#if !SHELL_ENABLE_VULKAN
  GTEST_SKIP();
#endif  // !SHELL_ENABLE_VULKAN
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell =
      CreateShell(settings,                                           //
                  GetTaskRunnersForFixture(),                         //
                  false,                                              //
                  nullptr,                                            //
                  false,                                              //
                  ShellTestPlatformView::BackendType::kVulkanBackend  //
      );
  ASSERT_NE(shell, nullptr);
  ASSERT_TRUE(shell->IsSetup());
  PlatformViewNotifyCreated(shell.get());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, CanCreateShellsWithMetalBackend) {
#if !SHELL_ENABLE_METAL
  GTEST_SKIP();
#endif  // !SHELL_ENABLE_METAL
  auto settings = CreateSettingsForFixture();
  std::unique_ptr<Shell> shell =
      CreateShell(settings,                                          //
                  GetTaskRunnersForFixture(),                        //
                  false,                                             //
                  nullptr,                                           //
                  false,                                             //
                  ShellTestPlatformView::BackendType::kMetalBackend  //
      );
  ASSERT_NE(shell, nullptr);
  ASSERT_TRUE(shell->IsSetup());
  PlatformViewNotifyCreated(shell.get());
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));
  PumpOneFrame(shell.get());
  PlatformViewNotifyDestroyed(shell.get());
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, UserTagSetOnStartup) {
  ASSERT_FALSE(DartVMRef::IsInstanceRunning());
  // Make sure the shell launch does not kick off the creation of the VM
  // instance by already creating one upfront.
  auto vm_settings = CreateSettingsForFixture();
  auto vm_ref = DartVMRef::Create(vm_settings);
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  auto settings = vm_settings;
  fml::AutoResetWaitableEvent isolate_create_latch;

  // ensure that "AppStartUpTag" is set during isolate creation.
  settings.root_isolate_create_callback = [&](const DartIsolate& isolate) {
    Dart_Handle current_tag = Dart_GetCurrentUserTag();
    Dart_Handle startup_tag = Dart_NewUserTag("AppStartUp");
    EXPECT_TRUE(Dart_IdentityEquals(current_tag, startup_tag));

    isolate_create_latch.Signal();
  };

  auto shell = CreateShell(settings);
  ASSERT_TRUE(ValidateShell(shell.get()));

  auto configuration = RunConfiguration::InferFromSettings(settings);
  ASSERT_TRUE(configuration.IsValid());

  RunEngine(shell.get(), std::move(configuration));
  ASSERT_TRUE(DartVMRef::IsInstanceRunning());

  DestroyShell(std::move(shell));
  isolate_create_latch.Wait();
}

TEST_F(ShellTest, PrefetchDefaultFontManager) {
  auto settings = CreateSettingsForFixture();
  settings.prefetched_default_font_manager = true;
  std::unique_ptr<Shell> shell;

  auto get_font_manager_count = [&] {
    fml::AutoResetWaitableEvent latch;
    size_t font_manager_count;
    fml::TaskRunner::RunNowOrPostTask(
        shell->GetTaskRunners().GetUITaskRunner(),
        [this, &shell, &latch, &font_manager_count]() {
          font_manager_count =
              GetFontCollection(shell.get())->GetFontManagersCount();
          latch.Signal();
        });
    latch.Wait();
    return font_manager_count;
  };
  size_t initial_font_manager_count = 0;
  settings.root_isolate_create_callback = [&](const auto& isolate) {
    ASSERT_GT(initial_font_manager_count, 0ul);
    // Should not have fetched the default font manager yet, since the root
    // isolate was only just created.
    ASSERT_EQ(get_font_manager_count(), initial_font_manager_count);
  };

  shell = CreateShell(std::move(settings));

  initial_font_manager_count = get_font_manager_count();

  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("emptyMain");
  RunEngine(shell.get(), std::move(configuration));

  // If the prefetched_default_font_manager flag is set, then the default font
  // manager will not be added until the engine starts running.
  ASSERT_EQ(get_font_manager_count(), initial_font_manager_count + 1);

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, OnPlatformViewCreatedWhenUIThreadIsBusy) {
  // This test will deadlock if the threading logic in
  // Shell::OnCreatePlatformView is wrong.
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(std::move(settings));

  fml::AutoResetWaitableEvent latch;
  fml::TaskRunner::RunNowOrPostTask(shell->GetTaskRunners().GetUITaskRunner(),
                                    [&latch]() { latch.Wait(); });

  ShellTest::PlatformViewNotifyCreated(shell.get());
  latch.Signal();

  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, UIWorkAfterOnPlatformViewDestroyed) {
  auto settings = CreateSettingsForFixture();
  auto shell = CreateShell(std::move(settings));
  auto configuration = RunConfiguration::InferFromSettings(settings);
  configuration.SetEntrypoint("drawFrames");

  fml::AutoResetWaitableEvent latch;
  fml::AutoResetWaitableEvent notify_native_latch;
  AddNativeCallback("NotifyNative", CREATE_NATIVE_ENTRY([&](auto args) {
                      notify_native_latch.Signal();
                      latch.Wait();
                    }));

  RunEngine(shell.get(), std::move(configuration));
  // Wait to make sure we get called back from Dart and thus have latched
  // the UI thread before we create/destroy the platform view.
  notify_native_latch.Wait();

  ShellTest::PlatformViewNotifyCreated(shell.get());

  fml::AutoResetWaitableEvent destroy_latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetPlatformTaskRunner(),
      [&shell, &destroy_latch]() {
        shell->GetPlatformView()->NotifyDestroyed();
        destroy_latch.Signal();
      });

  destroy_latch.Wait();

  // Unlatch the UI thread and let it send us a scene to render.
  latch.Signal();

  // Flush the UI task runner to make sure we process the render/scheduleFrame
  // request.
  fml::AutoResetWaitableEvent ui_flush_latch;
  fml::TaskRunner::RunNowOrPostTask(
      shell->GetTaskRunners().GetUITaskRunner(),
      [&ui_flush_latch]() { ui_flush_latch.Signal(); });
  ui_flush_latch.Wait();
  DestroyShell(std::move(shell));
}

TEST_F(ShellTest, UsesPlatformMessageHandler) {
  TaskRunners task_runners = GetTaskRunnersForFixture();
  auto settings = CreateSettingsForFixture();
  MockPlatformViewDelegate platform_view_delegate;
  auto platform_message_handler =
      std::make_shared<MockPlatformMessageHandler>();
  int message_id = 1;
  EXPECT_CALL(*platform_message_handler, HandlePlatformMessage(_));
  EXPECT_CALL(*platform_message_handler,
              InvokePlatformMessageEmptyResponseCallback(message_id));
  Shell::CreateCallback<PlatformView> platform_view_create_callback =
      [&platform_view_delegate, task_runners,
       platform_message_handler](flutter::Shell& shell) {
        auto result = std::make_unique<MockPlatformView>(platform_view_delegate,
                                                         task_runners);
        EXPECT_CALL(*result, GetPlatformMessageHandler())
            .WillOnce(Return(platform_message_handler));
        return result;
      };
  auto shell = CreateShell(
      /*settings=*/std::move(settings),
      /*task_runners=*/task_runners,
      /*simulate_vsync=*/false,
      /*shell_test_external_view_embedder=*/nullptr,
      /*is_gpu_disabled=*/false,
      /*rendering_backend=*/
      ShellTestPlatformView::BackendType::kDefaultBackend,
      /*platform_view_create_callback=*/platform_view_create_callback);

  EXPECT_EQ(platform_message_handler, shell->GetPlatformMessageHandler());
  PostSync(task_runners.GetUITaskRunner(), [&shell]() {
    size_t data_size = 4;
    fml::MallocMapping bytes =
        fml::MallocMapping(static_cast<uint8_t*>(malloc(data_size)), data_size);
    fml::RefPtr<MockPlatformMessageResponse> response =
        MockPlatformMessageResponse::Create();
    auto message = std::make_unique<PlatformMessage>(
        /*channel=*/"foo", /*data=*/std::move(bytes), /*response=*/response);
    (static_cast<Engine::Delegate*>(shell.get()))
        ->OnEngineHandlePlatformMessage(std::move(message));
  });
  shell->GetPlatformMessageHandler()
      ->InvokePlatformMessageEmptyResponseCallback(message_id);
  DestroyShell(std::move(shell));
}

}  // namespace testing
}  // namespace flutter
