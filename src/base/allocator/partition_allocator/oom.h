// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_OOM_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_OOM_H_

#include "base/allocator/partition_allocator/oom_callback.h"
#include "base/compiler_specific.h"
#include "base/immediate_crash.h"
#include "base/process/memory.h"
#include "build/build_config.h"

#if defined(OS_WIN)
#include <windows.h>
#endif

namespace {
// The crash is generated in a NOINLINE function so that we can classify the
// crash as an OOM solely by analyzing the stack trace. It is tagged as
// NOT_TAIL_CALLED to ensure that its parent function stays on the stack.
[[noreturn]] NOINLINE void NOT_TAIL_CALLED OnNoMemory(size_t size) {
  base::internal::RunPartitionAllocOomCallback();
  base::TerminateBecauseOutOfMemory(size);
  IMMEDIATE_CRASH();
}
}  // namespace

// OOM_CRASH(size) - Specialization of IMMEDIATE_CRASH which will raise a custom
// exception on Windows to signal this is OOM and not a normal assert.
// OOM_CRASH(size) is called by users of PageAllocator (including
// PartitionAlloc) to signify an allocation failure from the platform.
#define OOM_CRASH(size) \
  do {                  \
    OnNoMemory(size);   \
  } while (0)

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_OOM_H_
