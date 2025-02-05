// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_TEST_TRACE_TEST_UTILS_H_
#define BASE_TEST_TRACE_TEST_UTILS_H_

#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/trace_event/trace_log.h"

namespace base {
namespace tracing {
class PerfettoPlatform;
}

namespace test {

// A scoped class that sets up and tears down tracing support for unit tests.
// Note that only in-process tracing is supported by this harness. See
// //services/tracing for recording traces in multiprocess configurations.
class TracingEnvironment {
 public:
  explicit TracingEnvironment(TaskEnvironment&,
                              scoped_refptr<SequencedTaskRunner> =
                                  ThreadPool::CreateSequencedTaskRunner({}),
                              base::tracing::PerfettoPlatform* = nullptr);
  ~TracingEnvironment();

 private:
  TaskEnvironment& task_environment_;
};

}  // namespace test
}  // namespace base

#endif  // BASE_TEST_TRACE_TEST_UTILS_H_
