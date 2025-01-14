// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequence_manager/time_domain.h"

#include "base/task/sequence_manager/associated_thread_id.h"
#include "base/task/sequence_manager/sequence_manager_impl.h"
#include "base/task/sequence_manager/task_queue_impl.h"
#include "base/task/sequence_manager/work_queue.h"
#include "base/threading/thread_checker.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {
namespace sequence_manager {

TimeDomain::TimeDomain()
    : sequence_manager_(nullptr),
      associated_thread_(MakeRefCounted<internal::AssociatedThreadId>()) {}

TimeDomain::~TimeDomain() {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
}

void TimeDomain::OnRegisterWithSequenceManager(
    internal::SequenceManagerImpl* sequence_manager) {
  DCHECK(sequence_manager);
  DCHECK(!sequence_manager_);
  sequence_manager_ = sequence_manager;
  associated_thread_ = sequence_manager_->associated_thread();
}

SequenceManager* TimeDomain::sequence_manager() const {
  DCHECK(sequence_manager_);
  return sequence_manager_;
}

// TODO(kraynov): https://crbug.com/857101 Consider making an interface
// for SequenceManagerImpl which will expose SetNextDelayedDoWork and
// MaybeScheduleImmediateWork methods to make the functions below pure-virtual.

void TimeDomain::SetNextDelayedDoWork(LazyNow* lazy_now, TimeTicks run_time) {
  sequence_manager_->SetNextDelayedDoWork(lazy_now, run_time);
}

void TimeDomain::RequestDoWork() {
  sequence_manager_->ScheduleWork();
}

void TimeDomain::UnregisterQueue(internal::TaskQueueImpl* queue) {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
  DCHECK_EQ(queue->GetTimeDomain(), this);
  LazyNow lazy_now(CreateLazyNow());
  SetNextWakeUpForQueue(queue, absl::nullopt, &lazy_now);
}

void TimeDomain::SetNextWakeUpForQueue(
    internal::TaskQueueImpl* queue,
    absl::optional<internal::DelayedWakeUp> wake_up,
    LazyNow* lazy_now) {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
  DCHECK_EQ(queue->GetTimeDomain(), this);
  DCHECK(queue->IsQueueEnabled() || !wake_up);

  absl::optional<TimeTicks> previous_wake_up;
  absl::optional<internal::WakeUpResolution> previous_queue_resolution;
  if (!delayed_wake_up_queue_.empty())
    previous_wake_up = delayed_wake_up_queue_.Min().wake_up.time;
  if (queue->heap_handle().IsValid()) {
    previous_queue_resolution =
        delayed_wake_up_queue_.at(queue->heap_handle()).wake_up.resolution;
  }

  if (wake_up) {
    // Insert a new wake-up into the heap.
    if (queue->heap_handle().IsValid()) {
      // O(log n)
      delayed_wake_up_queue_.ChangeKey(queue->heap_handle(),
                                       {wake_up.value(), queue});
    } else {
      // O(log n)
      delayed_wake_up_queue_.insert({wake_up.value(), queue});
    }
  } else {
    // Remove a wake-up from heap if present.
    if (queue->heap_handle().IsValid())
      delayed_wake_up_queue_.erase(queue->heap_handle());
  }

  absl::optional<TimeTicks> new_wake_up;
  if (!delayed_wake_up_queue_.empty())
    new_wake_up = delayed_wake_up_queue_.Min().wake_up.time;

  if (previous_queue_resolution &&
      *previous_queue_resolution == internal::WakeUpResolution::kHigh) {
    pending_high_res_wake_up_count_--;
  }
  if (wake_up && wake_up->resolution == internal::WakeUpResolution::kHigh)
    pending_high_res_wake_up_count_++;
  DCHECK_GE(pending_high_res_wake_up_count_, 0);

  // TODO(kraynov): https://crbug.com/857101 Review the relationship with
  // SequenceManager's time. Right now it's not an issue since
  // VirtualTimeDomain doesn't invoke SequenceManager itself.

  if (new_wake_up == previous_wake_up) {
    // Nothing to be done
    return;
  }

  if (!new_wake_up) {
    // No new wake-up to be set, cancel the previous one.
    new_wake_up = TimeTicks::Max();
  }

  if (*new_wake_up <= lazy_now->Now()) {
    RequestDoWork();
  } else {
    SetNextDelayedDoWork(lazy_now, *new_wake_up);
  }
}

void TimeDomain::MoveReadyDelayedTasksToWorkQueues(LazyNow* lazy_now) {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
  // Wake up any queues with pending delayed work.  Note std::multimap stores
  // the elements sorted by key, so the begin() iterator points to the earliest
  // queue to wake-up.
  while (!delayed_wake_up_queue_.empty() &&
         delayed_wake_up_queue_.Min().wake_up.time <= lazy_now->Now()) {
    internal::TaskQueueImpl* queue = delayed_wake_up_queue_.Min().queue;
    queue->MoveReadyDelayedTasksToWorkQueue(lazy_now);
  }
}

absl::optional<TimeTicks> TimeDomain::NextScheduledRunTime() const {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
  if (delayed_wake_up_queue_.empty())
    return absl::nullopt;
  return delayed_wake_up_queue_.Min().wake_up.time;
}

Value TimeDomain::AsValue() const {
  Value state(Value::Type::DICTIONARY);
  state.SetStringKey("name", GetName());
  state.SetIntKey("registered_delay_count", delayed_wake_up_queue_.size());
  if (!delayed_wake_up_queue_.empty()) {
    TimeDelta delay = delayed_wake_up_queue_.Min().wake_up.time - Now();
    state.SetDoubleKey("next_delay_ms", delay.InMillisecondsF());
  }
  return state;
}

}  // namespace sequence_manager
}  // namespace base
