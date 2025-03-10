// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>

#include "flutter/fml/time/time_point.h"

#include "flutter/shell/platform/windows/task_runner.h"

#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {
class MockTaskRunner : public TaskRunner {
 public:
  MockTaskRunner(CurrentTimeProc get_current_time,
                 const TaskExpiredCallback& on_task_expired)
      : TaskRunner(get_current_time, on_task_expired) {}

  virtual bool RunsTasksOnCurrentThread() const override { return true; }

  void SimulateTimerAwake() { ProcessTasks(); }

 protected:
  virtual void WakeUp() override {
    // Do nothing to avoid processing tasks immediately after the tasks is
    // posted.
  }
};

uint64_t MockGetCurrentTime() {
  return static_cast<uint64_t>(
      fml::TimePoint::Now().ToEpochDelta().ToNanoseconds());
}
}  // namespace

TEST(TaskRunnerTest, MaybeExecuteTaskWithExactOrder) {
  std::vector<uint64_t> executed_task_order;
  auto runner =
      MockTaskRunner(MockGetCurrentTime,
                     [&executed_task_order](const FlutterTask* expired_task) {
                       executed_task_order.push_back(expired_task->task);
                     });

  uint64_t time_now = MockGetCurrentTime();

  runner.PostFlutterTask(FlutterTask{nullptr, 1}, time_now);
  runner.PostFlutterTask(FlutterTask{nullptr, 2}, time_now);
  runner.PostTask(
      [&executed_task_order]() { executed_task_order.push_back(3); });
  runner.PostTask(
      [&executed_task_order]() { executed_task_order.push_back(4); });

  runner.SimulateTimerAwake();

  std::vector<uint64_t> posted_task_order{1, 2, 3, 4};
  EXPECT_EQ(executed_task_order, posted_task_order);
}

TEST(TaskRunnerTest, MaybeExecuteTaskOnlyExpired) {
  std::set<uint64_t> executed_task;
  auto runner = MockTaskRunner(
      MockGetCurrentTime, [&executed_task](const FlutterTask* expired_task) {
        executed_task.insert(expired_task->task);
      });

  uint64_t time_now = MockGetCurrentTime();

  uint64_t task_expired_before_now = 1;
  uint64_t time_before_now = time_now - 10000;
  runner.PostFlutterTask(FlutterTask{nullptr, task_expired_before_now},
                         time_before_now);

  uint64_t task_expired_after_now = 2;
  uint64_t time_after_now = time_now + 10000;
  runner.PostFlutterTask(FlutterTask{nullptr, task_expired_after_now},
                         time_after_now);

  runner.SimulateTimerAwake();

  std::set<uint64_t> only_task_expired_before_now{task_expired_before_now};
  EXPECT_EQ(executed_task, only_task_expired_before_now);
}

}  // namespace testing
}  // namespace flutter
