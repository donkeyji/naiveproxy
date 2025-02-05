// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequence_manager/task_queue_impl.h"

#include <inttypes.h>

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/ranges/algorithm.h"
#include "base/strings/stringprintf.h"
#include "base/task/common/scoped_defer_task_posting.h"
#include "base/task/sequence_manager/sequence_manager_impl.h"
#include "base/task/sequence_manager/time_domain.h"
#include "base/task/sequence_manager/work_queue.h"
#include "base/task/task_observer.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/trace_event/base_tracing.h"
#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {
namespace sequence_manager {

// static
const char* TaskQueue::PriorityToString(TaskQueue::QueuePriority priority) {
  switch (priority) {
    case kControlPriority:
      return "control";
    case kHighestPriority:
      return "highest";
    case kVeryHighPriority:
      return "very_high";
    case kHighPriority:
      return "high";
    case kNormalPriority:
      return "normal";
    case kLowPriority:
      return "low";
    case kBestEffortPriority:
      return "best_effort";
    default:
      NOTREACHED();
      return nullptr;
  }
}

namespace internal {

TaskQueueImpl::GuardedTaskPoster::GuardedTaskPoster(TaskQueueImpl* outer)
    : outer_(outer) {}

TaskQueueImpl::GuardedTaskPoster::~GuardedTaskPoster() {}

bool TaskQueueImpl::GuardedTaskPoster::PostTask(PostedTask task) {
  // Do not process new PostTasks while we are handling a PostTask (tracing
  // has to do this) as it can lead to a deadlock and defer it instead.
  ScopedDeferTaskPosting disallow_task_posting;

  auto token = operations_controller_.TryBeginOperation();
  if (!token)
    return false;

  outer_->PostTask(std::move(task));
  return true;
}

TaskQueueImpl::TaskRunner::TaskRunner(
    scoped_refptr<GuardedTaskPoster> task_poster,
    scoped_refptr<AssociatedThreadId> associated_thread,
    TaskType task_type)
    : task_poster_(std::move(task_poster)),
      associated_thread_(std::move(associated_thread)),
      task_type_(task_type) {}

TaskQueueImpl::TaskRunner::~TaskRunner() {}

bool TaskQueueImpl::TaskRunner::PostDelayedTask(const Location& location,
                                                OnceClosure callback,
                                                TimeDelta delay) {
  return task_poster_->PostTask(PostedTask(this, std::move(callback), location,
                                           delay, Nestable::kNestable,
                                           task_type_));
}

bool TaskQueueImpl::TaskRunner::PostNonNestableDelayedTask(
    const Location& location,
    OnceClosure callback,
    TimeDelta delay) {
  return task_poster_->PostTask(PostedTask(this, std::move(callback), location,
                                           delay, Nestable::kNonNestable,
                                           task_type_));
}

bool TaskQueueImpl::TaskRunner::RunsTasksInCurrentSequence() const {
  return associated_thread_->IsBoundToCurrentThread();
}

TaskQueueImpl::TaskQueueImpl(SequenceManagerImpl* sequence_manager,
                             TimeDomain* time_domain,
                             const TaskQueue::Spec& spec)
    : name_(spec.name),
      sequence_manager_(sequence_manager),
      associated_thread_(sequence_manager
                             ? sequence_manager->associated_thread()
                             : AssociatedThreadId::CreateBound()),
      task_poster_(MakeRefCounted<GuardedTaskPoster>(this)),
      any_thread_(time_domain),
      main_thread_only_(this, time_domain),
      empty_queues_to_reload_handle_(
          sequence_manager
              ? sequence_manager->GetFlagToRequestReloadForEmptyQueue(this)
              : AtomicFlagSet::AtomicFlag()),
      should_monitor_quiescence_(spec.should_monitor_quiescence),
      should_notify_observers_(spec.should_notify_observers),
      delayed_fence_allowed_(spec.delayed_fence_allowed) {
  DCHECK(time_domain);
  UpdateCrossThreadQueueStateLocked();
  // SequenceManager can't be set later, so we need to prevent task runners
  // from posting any tasks.
  if (sequence_manager_)
    task_poster_->StartAcceptingOperations();
}

TaskQueueImpl::~TaskQueueImpl() {
#if DCHECK_IS_ON()
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  // NOTE this check shouldn't fire because |SequenceManagerImpl::queues_|
  // contains a strong reference to this TaskQueueImpl and the
  // SequenceManagerImpl destructor calls UnregisterTaskQueue on all task
  // queues.
  DCHECK(any_thread_.unregistered)
      << "UnregisterTaskQueue must be called first!";
#endif
}

TaskQueueImpl::AnyThread::AnyThread(TimeDomain* time_domain)
    : time_domain(time_domain) {}

TaskQueueImpl::AnyThread::~AnyThread() = default;

TaskQueueImpl::AnyThread::TracingOnly::TracingOnly() = default;
TaskQueueImpl::AnyThread::TracingOnly::~TracingOnly() = default;

TaskQueueImpl::MainThreadOnly::MainThreadOnly(TaskQueueImpl* task_queue,
                                              TimeDomain* time_domain)
    : time_domain(time_domain),
      delayed_work_queue(
          new WorkQueue(task_queue, "delayed", WorkQueue::QueueType::kDelayed)),
      immediate_work_queue(new WorkQueue(task_queue,
                                         "immediate",
                                         WorkQueue::QueueType::kImmediate)) {}

TaskQueueImpl::MainThreadOnly::~MainThreadOnly() = default;

scoped_refptr<SingleThreadTaskRunner> TaskQueueImpl::CreateTaskRunner(
    TaskType task_type) const {
  return MakeRefCounted<TaskRunner>(task_poster_, associated_thread_,
                                    task_type);
}

void TaskQueueImpl::UnregisterTaskQueue() {
  TRACE_EVENT0("base", "TaskQueueImpl::UnregisterTaskQueue");
  // Detach task runners.
  {
    ScopedAllowBaseSyncPrimitivesOutsideBlockingScope allow_wait;
    task_poster_->ShutdownAndWaitForZeroOperations();
  }

  TaskDeque immediate_incoming_queue;

  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    any_thread_.unregistered = true;
    any_thread_.time_domain = nullptr;
    immediate_incoming_queue.swap(any_thread_.immediate_incoming_queue);
    any_thread_.task_queue_observer = nullptr;
  }

  if (main_thread_only().time_domain)
    main_thread_only().time_domain->UnregisterQueue(this);

  main_thread_only().on_task_completed_handler = OnTaskCompletedHandler();
  main_thread_only().time_domain = nullptr;
  main_thread_only().task_queue_observer = nullptr;
  empty_queues_to_reload_handle_.ReleaseAtomicFlag();

  // It is possible for a task to hold a scoped_refptr to this, which
  // will lead to TaskQueueImpl destructor being called when deleting a task.
  // To avoid use-after-free, we need to clear all fields of a task queue
  // before starting to delete the tasks.
  // All work queues and priority queues containing tasks should be moved to
  // local variables on stack (std::move for unique_ptrs and swap for queues)
  // before clearing them and deleting tasks.

  // Flush the queues outside of the lock because TSAN complains about a lock
  // order inversion for tasks that are posted from within a lock, with a
  // destructor that acquires the same lock.

  DelayedIncomingQueue delayed_incoming_queue;
  delayed_incoming_queue.swap(&main_thread_only().delayed_incoming_queue);
  std::unique_ptr<WorkQueue> immediate_work_queue =
      std::move(main_thread_only().immediate_work_queue);
  std::unique_ptr<WorkQueue> delayed_work_queue =
      std::move(main_thread_only().delayed_work_queue);
}

const char* TaskQueueImpl::GetName() const {
  return name_;
}

void TaskQueueImpl::PostTask(PostedTask task) {
  CurrentThread current_thread =
      associated_thread_->IsBoundToCurrentThread()
          ? TaskQueueImpl::CurrentThread::kMainThread
          : TaskQueueImpl::CurrentThread::kNotMainThread;

#if DCHECK_IS_ON()
  MaybeLogPostTask(&task);
  MaybeAdjustTaskDelay(&task, current_thread);
#endif  // DCHECK_IS_ON()

  if (task.delay.is_zero()) {
    PostImmediateTaskImpl(std::move(task), current_thread);
  } else {
    PostDelayedTaskImpl(std::move(task), current_thread);
  }
}

void TaskQueueImpl::MaybeLogPostTask(PostedTask* task) {
#if DCHECK_IS_ON()
  if (!sequence_manager_->settings().log_post_task)
    return;

  LOG(INFO) << name_ << " PostTask " << task->location.ToString() << " delay "
            << task->delay;
#endif  // DCHECK_IS_ON()
}

void TaskQueueImpl::MaybeAdjustTaskDelay(PostedTask* task,
                                         CurrentThread current_thread) {
#if DCHECK_IS_ON()
  if (current_thread == TaskQueueImpl::CurrentThread::kNotMainThread) {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    // Add a per-priority delay to cross thread tasks. This can help diagnose
    // scheduler induced flakiness by making things flake most of the time.
    task->delay +=
        sequence_manager_->settings()
            .per_priority_cross_thread_task_delay[any_thread_.queue_set_index];
  } else {
    task->delay +=
        sequence_manager_->settings().per_priority_same_thread_task_delay
            [main_thread_only().immediate_work_queue->work_queue_set_index()];
  }
#endif  // DCHECK_IS_ON()
}

void TaskQueueImpl::PostImmediateTaskImpl(PostedTask task,
                                          CurrentThread current_thread) {
  // Use CHECK instead of DCHECK to crash earlier. See http://crbug.com/711167
  // for details.
  CHECK(task.callback);

  bool should_schedule_work = false;
  {
    // TODO(alexclarke): Maybe add a main thread only immediate_incoming_queue
    // See https://crbug.com/901800
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    LazyNow lazy_now = any_thread_.time_domain->CreateLazyNow();
    bool add_queue_time_to_tasks = sequence_manager_->GetAddQueueTimeToTasks();
    if (add_queue_time_to_tasks || delayed_fence_allowed_)
      task.queue_time = lazy_now.Now();

    // The sequence number must be incremented atomically with pushing onto the
    // incoming queue. Otherwise if there are several threads posting task we
    // risk breaking the assumption that sequence numbers increase monotonically
    // within a queue.
    EnqueueOrder sequence_number = sequence_manager_->GetNextSequenceNumber();
    bool was_immediate_incoming_queue_empty =
        any_thread_.immediate_incoming_queue.empty();
    // Delayed run time is null for an immediate task.
    base::TimeTicks delayed_run_time;
    any_thread_.immediate_incoming_queue.push_back(Task(
        std::move(task), delayed_run_time, sequence_number, sequence_number));

#if DCHECK_IS_ON()
    any_thread_.immediate_incoming_queue.back().cross_thread_ =
        (current_thread == TaskQueueImpl::CurrentThread::kNotMainThread);
#endif

    sequence_manager_->WillQueueTask(
        &any_thread_.immediate_incoming_queue.back(), name_);
    MaybeReportIpcTaskQueuedFromAnyThreadLocked(
        &any_thread_.immediate_incoming_queue.back(), name_);
    if (!any_thread_.on_task_posted_handler.is_null()) {
      any_thread_.on_task_posted_handler.Run(
          any_thread_.immediate_incoming_queue.back());
    }

    // If this queue was completely empty, then the SequenceManager needs to be
    // informed so it can reload the work queue and add us to the
    // TaskQueueSelector which can only be done from the main thread. In
    // addition it may need to schedule a DoWork if this queue isn't blocked.
    if (was_immediate_incoming_queue_empty &&
        any_thread_.immediate_work_queue_empty) {
      empty_queues_to_reload_handle_.SetActive(true);
      should_schedule_work =
          any_thread_.post_immediate_task_should_schedule_work;
    }
  }

  // On windows it's important to call this outside of a lock because calling a
  // pump while holding a lock can result in priority inversions. See
  // http://shortn/_ntnKNqjDQT for a discussion.
  //
  // Calling ScheduleWork outside the lock should be safe, only the main thread
  // can mutate |any_thread_.post_immediate_task_should_schedule_work|. If it
  // transitions to false we call ScheduleWork redundantly that's harmless. If
  // it transitions to true, the side effect of
  // |empty_queues_to_reload_handle_SetActive(true)| is guaranteed to be picked
  // up by the ThreadController's call to SequenceManagerImpl::DelayTillNextTask
  // when it computes what continuation (if any) is needed.
  if (should_schedule_work)
    sequence_manager_->ScheduleWork();

  TraceQueueSize();
}

void TaskQueueImpl::PostDelayedTaskImpl(PostedTask posted_task,
                                        CurrentThread current_thread) {
  // Use CHECK instead of DCHECK to crash earlier. See http://crbug.com/711167
  // for details.
  CHECK(posted_task.callback);
  DCHECK_GT(posted_task.delay, TimeDelta());

  WakeUpResolution resolution = WakeUpResolution::kLow;
#if defined(OS_WIN)
  // We consider the task needs a high resolution timer if the delay is more
  // than 0 and less than 32ms. This caps the relative error to less than 50% :
  // a 33ms wait can wake at 48ms since the default resolution on Windows is
  // between 10 and 15ms.
  if (posted_task.delay.InMilliseconds() <
      (2 * Time::kMinLowResolutionThresholdMs))
    resolution = WakeUpResolution::kHigh;
#endif  // defined(OS_WIN)

  if (current_thread == CurrentThread::kMainThread) {
    // Lock-free fast path for delayed tasks posted from the main thread.
    EnqueueOrder sequence_number = sequence_manager_->GetNextSequenceNumber();

    TimeTicks time_domain_now = main_thread_only().time_domain->Now();
    TimeTicks time_domain_delayed_run_time =
        time_domain_now + posted_task.delay;
    if (sequence_manager_->GetAddQueueTimeToTasks())
      posted_task.queue_time = time_domain_now;

    PushOntoDelayedIncomingQueueFromMainThread(
        Task(std::move(posted_task), time_domain_delayed_run_time,
             sequence_number, EnqueueOrder(), resolution),
        time_domain_now, /* notify_task_annotator */ true);
  } else {
    // NOTE posting a delayed task from a different thread is not expected to
    // be common. This pathway is less optimal than perhaps it could be
    // because it causes two main thread tasks to be run.  Should this
    // assumption prove to be false in future, we may need to revisit this.
    EnqueueOrder sequence_number = sequence_manager_->GetNextSequenceNumber();

    TimeTicks time_domain_now;
    {
      base::internal::CheckedAutoLock lock(any_thread_lock_);
      time_domain_now = any_thread_.time_domain->Now();
    }
    TimeTicks time_domain_delayed_run_time =
        time_domain_now + posted_task.delay;
    if (sequence_manager_->GetAddQueueTimeToTasks())
      posted_task.queue_time = time_domain_now;

    PushOntoDelayedIncomingQueue(
        Task(std::move(posted_task), time_domain_delayed_run_time,
             sequence_number, EnqueueOrder(), resolution));
  }
}

void TaskQueueImpl::PushOntoDelayedIncomingQueueFromMainThread(
    Task pending_task,
    TimeTicks now,
    bool notify_task_annotator) {
#if DCHECK_IS_ON()
  pending_task.cross_thread_ = false;
#endif

  if (notify_task_annotator) {
    sequence_manager_->WillQueueTask(&pending_task, name_);
    MaybeReportIpcTaskQueuedFromMainThread(&pending_task, name_);
  }
  main_thread_only().delayed_incoming_queue.push(std::move(pending_task));

  LazyNow lazy_now(now);
  UpdateDelayedWakeUp(&lazy_now);

  TraceQueueSize();
}

void TaskQueueImpl::PushOntoDelayedIncomingQueue(Task pending_task) {
  sequence_manager_->WillQueueTask(&pending_task, name_);
  MaybeReportIpcTaskQueuedFromAnyThreadUnlocked(&pending_task, name_);

#if DCHECK_IS_ON()
  pending_task.cross_thread_ = true;
#endif

  // TODO(altimin): Add a copy method to Task to capture metadata here.
  auto task_runner = pending_task.task_runner;
  const auto task_type = pending_task.task_type;
  PostImmediateTaskImpl(
      PostedTask(std::move(task_runner),
                 BindOnce(&TaskQueueImpl::ScheduleDelayedWorkTask,
                          Unretained(this), std::move(pending_task)),
                 FROM_HERE, TimeDelta(), Nestable::kNonNestable, task_type),
      CurrentThread::kNotMainThread);
}

void TaskQueueImpl::ScheduleDelayedWorkTask(Task pending_task) {
  DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
  TimeTicks delayed_run_time = pending_task.delayed_run_time;
  TimeTicks time_domain_now = main_thread_only().time_domain->Now();
  if (delayed_run_time <= time_domain_now) {
    // If |delayed_run_time| is in the past then push it onto the work queue
    // immediately. To ensure the right task ordering we need to temporarily
    // push it onto the |delayed_incoming_queue|.
    pending_task.delayed_run_time = time_domain_now;
    main_thread_only().delayed_incoming_queue.push(std::move(pending_task));
    LazyNow lazy_now(time_domain_now);
    MoveReadyDelayedTasksToWorkQueue(&lazy_now);
  } else {
    // If |delayed_run_time| is in the future we can queue it as normal.
    PushOntoDelayedIncomingQueueFromMainThread(std::move(pending_task),
                                               time_domain_now, false);
  }
  TraceQueueSize();
}

void TaskQueueImpl::ReloadEmptyImmediateWorkQueue() {
  DCHECK(main_thread_only().immediate_work_queue->Empty());
  main_thread_only().immediate_work_queue->TakeImmediateIncomingQueueTasks();

  if (main_thread_only().task_queue_observer && IsQueueEnabled()) {
    main_thread_only().task_queue_observer->OnQueueNextWakeUpChanged(
        TimeTicks());
  }
}

void TaskQueueImpl::TakeImmediateIncomingQueueTasks(TaskDeque* queue) {
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  DCHECK(queue->empty());
  queue->swap(any_thread_.immediate_incoming_queue);

  // Since |immediate_incoming_queue| is empty, now is a good time to consider
  // reducing it's capacity if we're wasting memory.
  any_thread_.immediate_incoming_queue.MaybeShrinkQueue();

  // Activate delayed fence if necessary. This is ideologically similar to
  // ActivateDelayedFenceIfNeeded, but due to immediate tasks being posted
  // from any thread we can't generate an enqueue order for the fence there,
  // so we have to check all immediate tasks and use their enqueue order for
  // a fence.
  if (main_thread_only().delayed_fence) {
    for (const Task& task : *queue) {
      DCHECK(!task.queue_time.is_null());
      DCHECK(task.delayed_run_time.is_null());
      if (task.queue_time >= main_thread_only().delayed_fence.value()) {
        main_thread_only().delayed_fence = absl::nullopt;
        DCHECK(!main_thread_only().current_fence);
        main_thread_only().current_fence = task.enqueue_order();
        // Do not trigger WorkQueueSets notification when taking incoming
        // immediate queue.
        main_thread_only().immediate_work_queue->InsertFenceSilently(
            main_thread_only().current_fence);
        main_thread_only().delayed_work_queue->InsertFenceSilently(
            main_thread_only().current_fence);
        break;
      }
    }
  }

  UpdateCrossThreadQueueStateLocked();
}

bool TaskQueueImpl::IsEmpty() const {
  if (!main_thread_only().delayed_work_queue->Empty() ||
      !main_thread_only().delayed_incoming_queue.empty() ||
      !main_thread_only().immediate_work_queue->Empty()) {
    return false;
  }

  base::internal::CheckedAutoLock lock(any_thread_lock_);
  return any_thread_.immediate_incoming_queue.empty();
}

size_t TaskQueueImpl::GetNumberOfPendingTasks() const {
  size_t task_count = 0;
  task_count += main_thread_only().delayed_work_queue->Size();
  task_count += main_thread_only().delayed_incoming_queue.size();
  task_count += main_thread_only().immediate_work_queue->Size();

  base::internal::CheckedAutoLock lock(any_thread_lock_);
  task_count += any_thread_.immediate_incoming_queue.size();
  return task_count;
}

bool TaskQueueImpl::HasTaskToRunImmediately() const {
  // Any work queue tasks count as immediate work.
  if (!main_thread_only().delayed_work_queue->Empty() ||
      !main_thread_only().immediate_work_queue->Empty()) {
    return true;
  }

  // Tasks on |delayed_incoming_queue| that could run now, count as
  // immediate work.
  if (!main_thread_only().delayed_incoming_queue.empty() &&
      main_thread_only().delayed_incoming_queue.top().delayed_run_time <=
          main_thread_only().time_domain->CreateLazyNow().Now()) {
    return true;
  }

  // Finally tasks on |immediate_incoming_queue| count as immediate work.
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  return !any_thread_.immediate_incoming_queue.empty();
}

absl::optional<DelayedWakeUp> TaskQueueImpl::GetNextScheduledWakeUpImpl() {
  // Note we don't scheduled a wake-up for disabled queues.
  if (main_thread_only().delayed_incoming_queue.empty() || !IsQueueEnabled())
    return absl::nullopt;

  // High resolution is needed if the queue contains high resolution tasks and
  // has a priority index <= kNormalPriority (precise execution time is
  // unnecessary for a low priority queue).
  WakeUpResolution resolution =
      has_pending_high_resolution_tasks() &&
              GetQueuePriority() <= TaskQueue::QueuePriority::kNormalPriority
          ? WakeUpResolution::kHigh
          : WakeUpResolution::kLow;

  const auto& top_task = main_thread_only().delayed_incoming_queue.top();
  return DelayedWakeUp{top_task.delayed_run_time, top_task.sequence_num,
                       resolution};
}

absl::optional<TimeTicks> TaskQueueImpl::GetNextScheduledWakeUp() {
  absl::optional<DelayedWakeUp> wake_up = GetNextScheduledWakeUpImpl();
  if (!wake_up)
    return absl::nullopt;
  return wake_up->time;
}

void TaskQueueImpl::MoveReadyDelayedTasksToWorkQueue(LazyNow* lazy_now) {
  // Enqueue all delayed tasks that should be running now, skipping any that
  // have been canceled.
  WorkQueue::TaskPusher delayed_work_queue_task_pusher(
      main_thread_only().delayed_work_queue->CreateTaskPusher());

  while (!main_thread_only().delayed_incoming_queue.empty()) {
    Task* task =
        const_cast<Task*>(&main_thread_only().delayed_incoming_queue.top());
    // TODO(crbug.com/990245): Remove after the crash has been resolved.
    sequence_manager_->RecordCrashKeys(*task);
    if (!task->task || task->task.IsCancelled()) {
      main_thread_only().delayed_incoming_queue.pop();
      continue;
    }
    if (task->delayed_run_time > lazy_now->Now())
      break;
#if DCHECK_IS_ON()
    if (sequence_manager_->settings().log_task_delay_expiry)
      VLOG(0) << name_ << " Delay expired for " << task->posted_from.ToString();
#endif  // DCHECK_IS_ON()
    DCHECK(!task->delayed_run_time.is_null());
    ActivateDelayedFenceIfNeeded(task->delayed_run_time);
    DCHECK(!task->enqueue_order_set());
    task->set_enqueue_order(sequence_manager_->GetNextSequenceNumber());

    delayed_work_queue_task_pusher.Push(task);
    main_thread_only().delayed_incoming_queue.pop();
  }

  UpdateDelayedWakeUp(lazy_now);
}

void TaskQueueImpl::TraceQueueSize() const {
  bool is_tracing;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(
      TRACE_DISABLED_BY_DEFAULT("sequence_manager"), &is_tracing);
  if (!is_tracing)
    return;

  // It's only safe to access the work queues from the main thread.
  // TODO(alexclarke): We should find another way of tracing this
  if (!associated_thread_->IsBoundToCurrentThread())
    return;

  size_t total_task_count;
  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    total_task_count = any_thread_.immediate_incoming_queue.size() +
                       main_thread_only().immediate_work_queue->Size() +
                       main_thread_only().delayed_work_queue->Size() +
                       main_thread_only().delayed_incoming_queue.size();
  }
  TRACE_COUNTER1(TRACE_DISABLED_BY_DEFAULT("sequence_manager"), GetName(),
                 total_task_count);
}

void TaskQueueImpl::SetQueuePriority(TaskQueue::QueuePriority priority) {
  const TaskQueue::QueuePriority previous_priority = GetQueuePriority();
  if (priority == previous_priority)
    return;
  sequence_manager_->main_thread_only().selector.SetQueuePriority(this,
                                                                  priority);

#if defined(OS_WIN)
  // Updating queue priority can change whether high resolution timer is needed.
  LazyNow lazy_now = main_thread_only().time_domain->CreateLazyNow();
  UpdateDelayedWakeUp(&lazy_now);
#endif

  static_assert(TaskQueue::QueuePriority::kLowPriority >
                    TaskQueue::QueuePriority::kNormalPriority,
                "Priorities are not ordered as expected");
  if (priority > TaskQueue::QueuePriority::kNormalPriority) {
    // |priority| is now kLowPriority or less important so update accordingly.
    main_thread_only()
        .enqueue_order_at_which_we_became_unblocked_with_normal_priority =
        EnqueueOrder::max();
  } else if (previous_priority > TaskQueue::QueuePriority::kNormalPriority) {
    // |priority| is no longer kLowPriority or less important so record current
    // sequence number.
    DCHECK_EQ(
        main_thread_only()
            .enqueue_order_at_which_we_became_unblocked_with_normal_priority,
        EnqueueOrder::max());
    main_thread_only()
        .enqueue_order_at_which_we_became_unblocked_with_normal_priority =
        sequence_manager_->GetNextSequenceNumber();
  }
}

TaskQueue::QueuePriority TaskQueueImpl::GetQueuePriority() const {
  size_t set_index = immediate_work_queue()->work_queue_set_index();
  DCHECK_EQ(set_index, delayed_work_queue()->work_queue_set_index());
  return static_cast<TaskQueue::QueuePriority>(set_index);
}

Value TaskQueueImpl::AsValue(TimeTicks now, bool force_verbose) const {
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  Value state(Value::Type::DICTIONARY);
  state.SetStringKey("name", GetName());
  if (any_thread_.unregistered) {
    state.SetBoolKey("unregistered", true);
    return state;
  }
  DCHECK(main_thread_only().time_domain);
  DCHECK(main_thread_only().delayed_work_queue);
  DCHECK(main_thread_only().immediate_work_queue);

  state.SetStringKey(
      "task_queue_id",
      StringPrintf("0x%" PRIx64,
                   static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this))));
  state.SetBoolKey("enabled", IsQueueEnabled());
  state.SetStringKey("time_domain_name",
                     main_thread_only().time_domain->GetName());
  state.SetIntKey("any_thread_.immediate_incoming_queuesize",
                  any_thread_.immediate_incoming_queue.size());
  state.SetIntKey("delayed_incoming_queue_size",
                  main_thread_only().delayed_incoming_queue.size());
  state.SetIntKey("immediate_work_queue_size",
                  main_thread_only().immediate_work_queue->Size());
  state.SetIntKey("delayed_work_queue_size",
                  main_thread_only().delayed_work_queue->Size());

  state.SetIntKey("any_thread_.immediate_incoming_queuecapacity",
                  any_thread_.immediate_incoming_queue.capacity());
  state.SetIntKey("immediate_work_queue_capacity",
                  immediate_work_queue()->Capacity());
  state.SetIntKey("delayed_work_queue_capacity",
                  delayed_work_queue()->Capacity());

  if (!main_thread_only().delayed_incoming_queue.empty()) {
    TimeDelta delay_to_next_task =
        (main_thread_only().delayed_incoming_queue.top().delayed_run_time -
         main_thread_only().time_domain->CreateLazyNow().Now());
    state.SetDoubleKey("delay_to_next_task_ms",
                       delay_to_next_task.InMillisecondsF());
  }
  if (main_thread_only().current_fence)
    state.SetIntKey("current_fence", main_thread_only().current_fence);
  if (main_thread_only().delayed_fence) {
    state.SetDoubleKey(
        "delayed_fence_seconds_from_now",
        (main_thread_only().delayed_fence.value() - now).InSecondsF());
  }

  bool verbose = false;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(
      TRACE_DISABLED_BY_DEFAULT("sequence_manager.verbose_snapshots"),
      &verbose);

  if (verbose || force_verbose) {
    state.SetKey("immediate_incoming_queue",
                 QueueAsValue(any_thread_.immediate_incoming_queue, now));
    state.SetKey("delayed_work_queue",
                 main_thread_only().delayed_work_queue->AsValue(now));
    state.SetKey("immediate_work_queue",
                 main_thread_only().immediate_work_queue->AsValue(now));
    state.SetKey("delayed_incoming_queue",
                 main_thread_only().delayed_incoming_queue.AsValue(now));
  }
  state.SetStringKey("priority",
                     TaskQueue::PriorityToString(GetQueuePriority()));
  return state;
}

void TaskQueueImpl::AddTaskObserver(TaskObserver* task_observer) {
  main_thread_only().task_observers.AddObserver(task_observer);
}

void TaskQueueImpl::RemoveTaskObserver(TaskObserver* task_observer) {
  main_thread_only().task_observers.RemoveObserver(task_observer);
}

void TaskQueueImpl::NotifyWillProcessTask(const Task& task,
                                          bool was_blocked_or_low_priority) {
  DCHECK(should_notify_observers_);

  if (main_thread_only().blame_context)
    main_thread_only().blame_context->Enter();

  for (auto& observer : main_thread_only().task_observers)
    observer.WillProcessTask(task, was_blocked_or_low_priority);
}

void TaskQueueImpl::NotifyDidProcessTask(const Task& task) {
  DCHECK(should_notify_observers_);
  for (auto& observer : main_thread_only().task_observers)
    observer.DidProcessTask(task);
  if (main_thread_only().blame_context)
    main_thread_only().blame_context->Leave();
}

void TaskQueueImpl::SetTimeDomain(TimeDomain* time_domain) {
  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    DCHECK(time_domain);
    DCHECK(!any_thread_.unregistered);
    if (any_thread_.unregistered)
      return;
    DCHECK_CALLED_ON_VALID_THREAD(associated_thread_->thread_checker);
    if (time_domain == main_thread_only().time_domain)
      return;

    any_thread_.time_domain = time_domain;
  }

  main_thread_only().time_domain->UnregisterQueue(this);
  main_thread_only().time_domain = time_domain;

  LazyNow lazy_now = time_domain->CreateLazyNow();
  // Clear scheduled wake up to ensure that new notifications are issued
  // correctly.
  // TODO(altimin): Remove this when we won't have to support changing time
  // domains.
  main_thread_only().scheduled_wake_up = absl::nullopt;
  UpdateDelayedWakeUp(&lazy_now);
}

TimeDomain* TaskQueueImpl::GetTimeDomain() const {
  DCHECK(associated_thread_->IsBoundToCurrentThread() ||
         !associated_thread_->IsBound());
  return main_thread_only().time_domain;
}

void TaskQueueImpl::SetBlameContext(trace_event::BlameContext* blame_context) {
  main_thread_only().blame_context = blame_context;
}

void TaskQueueImpl::InsertFence(TaskQueue::InsertFencePosition position) {
  // Only one fence may be present at a time.
  main_thread_only().delayed_fence = absl::nullopt;

  EnqueueOrder previous_fence = main_thread_only().current_fence;
  EnqueueOrder current_fence = position == TaskQueue::InsertFencePosition::kNow
                                   ? sequence_manager_->GetNextSequenceNumber()
                                   : EnqueueOrder::blocking_fence();

  // Tasks posted after this point will have a strictly higher enqueue order
  // and will be blocked from running.
  main_thread_only().current_fence = current_fence;
  bool front_task_unblocked =
      main_thread_only().immediate_work_queue->InsertFence(current_fence);
  front_task_unblocked |=
      main_thread_only().delayed_work_queue->InsertFence(current_fence);

  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    if (!front_task_unblocked && previous_fence &&
        previous_fence < current_fence) {
      if (!any_thread_.immediate_incoming_queue.empty() &&
          any_thread_.immediate_incoming_queue.front().enqueue_order() >
              previous_fence &&
          any_thread_.immediate_incoming_queue.front().enqueue_order() <
              current_fence) {
        front_task_unblocked = true;
      }
    }

    UpdateCrossThreadQueueStateLocked();
  }

  if (IsQueueEnabled() && front_task_unblocked) {
    OnQueueUnblocked();
    sequence_manager_->ScheduleWork();
  }
}

void TaskQueueImpl::InsertFenceAt(TimeTicks time) {
  DCHECK(delayed_fence_allowed_)
      << "Delayed fences are not supported for this queue. Enable them "
         "explicitly in TaskQueue::Spec when creating the queue";

  // Task queue can have only one fence, delayed or not.
  RemoveFence();
  main_thread_only().delayed_fence = time;
}

void TaskQueueImpl::RemoveFence() {
  EnqueueOrder previous_fence = main_thread_only().current_fence;
  main_thread_only().current_fence = EnqueueOrder::none();
  main_thread_only().delayed_fence = absl::nullopt;

  bool front_task_unblocked =
      main_thread_only().immediate_work_queue->RemoveFence();
  front_task_unblocked |= main_thread_only().delayed_work_queue->RemoveFence();

  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    if (!front_task_unblocked && previous_fence) {
      if (!any_thread_.immediate_incoming_queue.empty() &&
          any_thread_.immediate_incoming_queue.front().enqueue_order() >
              previous_fence) {
        front_task_unblocked = true;
      }
    }

    UpdateCrossThreadQueueStateLocked();
  }

  if (IsQueueEnabled() && front_task_unblocked) {
    OnQueueUnblocked();
    sequence_manager_->ScheduleWork();
  }
}

bool TaskQueueImpl::BlockedByFence() const {
  if (!main_thread_only().current_fence)
    return false;

  if (!main_thread_only().immediate_work_queue->BlockedByFence() ||
      !main_thread_only().delayed_work_queue->BlockedByFence()) {
    return false;
  }

  base::internal::CheckedAutoLock lock(any_thread_lock_);
  if (any_thread_.immediate_incoming_queue.empty())
    return true;

  return any_thread_.immediate_incoming_queue.front().enqueue_order() >
         main_thread_only().current_fence;
}

bool TaskQueueImpl::HasActiveFence() {
  if (main_thread_only().delayed_fence &&
      main_thread_only().time_domain->Now() >
          main_thread_only().delayed_fence.value()) {
    return true;
  }
  return !!main_thread_only().current_fence;
}

bool TaskQueueImpl::CouldTaskRun(EnqueueOrder enqueue_order) const {
  if (!IsQueueEnabled())
    return false;

  if (!main_thread_only().current_fence)
    return true;

  return enqueue_order < main_thread_only().current_fence;
}

bool TaskQueueImpl::WasBlockedOrLowPriority(EnqueueOrder enqueue_order) const {
  return enqueue_order <
         main_thread_only()
             .enqueue_order_at_which_we_became_unblocked_with_normal_priority;
}

// static
Value TaskQueueImpl::QueueAsValue(const TaskDeque& queue, TimeTicks now) {
  Value state(Value::Type::LIST);
  for (const Task& task : queue)
    state.Append(TaskAsValue(task, now));
  return state;
}

// static
Value TaskQueueImpl::TaskAsValue(const Task& task, TimeTicks now) {
  Value state(Value::Type::DICTIONARY);
  state.SetStringKey("posted_from", task.posted_from.ToString());
  if (task.enqueue_order_set())
    state.SetIntKey("enqueue_order", task.enqueue_order());
  state.SetIntKey("sequence_num", task.sequence_num);
  state.SetBoolKey("nestable", task.nestable == Nestable::kNestable);
  state.SetBoolKey("is_high_res", task.is_high_res);
  state.SetBoolKey("is_cancelled", task.task.IsCancelled());
  state.SetDoubleKey("delayed_run_time",
                     (task.delayed_run_time - TimeTicks()).InMillisecondsF());
  const TimeDelta delayed_run_time_milliseconds_from_now =
      task.delayed_run_time.is_null() ? TimeDelta()
                                      : (task.delayed_run_time - now);
  state.SetDoubleKey("delayed_run_time_milliseconds_from_now",
                     delayed_run_time_milliseconds_from_now.InMillisecondsF());
  return state;
}

bool TaskQueueImpl::IsQueueEnabled() const {
  return main_thread_only().is_enabled;
}

void TaskQueueImpl::SetQueueEnabled(bool enabled) {
  if (main_thread_only().is_enabled == enabled)
    return;

  // Update the |main_thread_only_| struct.
  main_thread_only().is_enabled = enabled;
  main_thread_only().disabled_time = absl::nullopt;
  if (!enabled) {
    bool tracing_enabled = false;
    TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("lifecycles"),
                                       &tracing_enabled);
    main_thread_only().disabled_time = main_thread_only().time_domain->Now();
  } else {
    // Override reporting if the queue is becoming enabled again.
    main_thread_only().should_report_posted_tasks_when_disabled = false;
  }

  LazyNow lazy_now = main_thread_only().time_domain->CreateLazyNow();
  UpdateDelayedWakeUp(&lazy_now);

  bool has_pending_immediate_work = false;

  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    UpdateCrossThreadQueueStateLocked();
    has_pending_immediate_work = HasPendingImmediateWorkLocked();

    // Copy over the task-reporting related state.
    any_thread_.tracing_only.is_enabled = enabled;
    any_thread_.tracing_only.disabled_time = main_thread_only().disabled_time;
    any_thread_.tracing_only.should_report_posted_tasks_when_disabled =
        main_thread_only().should_report_posted_tasks_when_disabled;
  }

  // |sequence_manager_| can be null in tests.
  if (!sequence_manager_)
    return;

  // Finally, enable or disable the queue with the selector.
  if (enabled) {
    if (has_pending_immediate_work && main_thread_only().task_queue_observer) {
      // Delayed work notification will be issued via time domain.
      main_thread_only().task_queue_observer->OnQueueNextWakeUpChanged(
          TimeTicks());
    }

    // Note the selector calls SequenceManager::OnTaskQueueEnabled which posts
    // a DoWork if needed.
    sequence_manager_->main_thread_only().selector.EnableQueue(this);

    if (!BlockedByFence())
      OnQueueUnblocked();
  } else {
    sequence_manager_->main_thread_only().selector.DisableQueue(this);
  }
}

void TaskQueueImpl::SetShouldReportPostedTasksWhenDisabled(bool should_report) {
  if (main_thread_only().should_report_posted_tasks_when_disabled ==
      should_report)
    return;

  // Only observe transitions turning the reporting on if tracing is enabled.
  if (should_report) {
    bool tracing_enabled = false;
    TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("lifecycles"),
                                       &tracing_enabled);
    if (!tracing_enabled)
      return;
  }

  main_thread_only().should_report_posted_tasks_when_disabled = should_report;

  // Mirror the state to the AnyThread struct as well.
  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    any_thread_.tracing_only.should_report_posted_tasks_when_disabled =
        should_report;
  }
}

void TaskQueueImpl::UpdateCrossThreadQueueStateLocked() {
  any_thread_.immediate_work_queue_empty =
      main_thread_only().immediate_work_queue->Empty();

  if (main_thread_only().task_queue_observer) {
    // If there's an observer we need a DoWork for the callback to be issued by
    // ReloadEmptyImmediateWorkQueue. The callback isn't sent for disabled
    // queues.
    any_thread_.post_immediate_task_should_schedule_work = IsQueueEnabled();
  } else {
    // Otherwise we need PostImmediateTaskImpl to ScheduleWork unless the queue
    // is blocked or disabled.
    any_thread_.post_immediate_task_should_schedule_work =
        IsQueueEnabled() && !main_thread_only().current_fence;
  }

#if DCHECK_IS_ON()
  any_thread_.queue_set_index =
      main_thread_only().immediate_work_queue->work_queue_set_index();
#endif
}

void TaskQueueImpl::ReclaimMemory(TimeTicks now) {
  if (main_thread_only().delayed_incoming_queue.empty())
    return;
  main_thread_only().delayed_incoming_queue.SweepCancelledTasks(
      sequence_manager_);

  // Also consider shrinking the work queue if it's wasting memory.
  main_thread_only().delayed_work_queue->MaybeShrinkQueue();
  main_thread_only().immediate_work_queue->MaybeShrinkQueue();

  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    any_thread_.immediate_incoming_queue.MaybeShrinkQueue();
  }

  LazyNow lazy_now(now);
  UpdateDelayedWakeUp(&lazy_now);
}

void TaskQueueImpl::PushImmediateIncomingTaskForTest(Task&& task) {
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  any_thread_.immediate_incoming_queue.push_back(std::move(task));
}

void TaskQueueImpl::RequeueDeferredNonNestableTask(
    DeferredNonNestableTask task) {
  DCHECK(task.task.nestable == Nestable::kNonNestable);
  // The re-queued tasks have to be pushed onto the front because we'd otherwise
  // violate the strict monotonically increasing enqueue order within the
  // WorkQueue.  We can't assign them a new enqueue order here because that will
  // not behave correctly with fences and things will break (e.g Idle TQ).
  if (task.work_queue_type == WorkQueueType::kDelayed) {
    main_thread_only().delayed_work_queue->PushNonNestableTaskToFront(
        std::move(task.task));
  } else {
    // We're about to push |task| onto an empty |immediate_work_queue|
    // (bypassing |immediate_incoming_queue_|). As such, we no longer need to
    // reload if we were planning to. The flag must be cleared while holding
    // the lock to avoid a cross-thread post task setting it again before
    // we actually make |immediate_work_queue| non-empty.
    if (main_thread_only().immediate_work_queue->Empty()) {
      base::internal::CheckedAutoLock lock(any_thread_lock_);
      empty_queues_to_reload_handle_.SetActive(false);

      any_thread_.immediate_work_queue_empty = false;
      main_thread_only().immediate_work_queue->PushNonNestableTaskToFront(
          std::move(task.task));

    } else {
      main_thread_only().immediate_work_queue->PushNonNestableTaskToFront(
          std::move(task.task));
    }
  }
}

void TaskQueueImpl::SetObserver(TaskQueue::Observer* observer) {
  if (observer) {
    DCHECK(!main_thread_only().task_queue_observer)
        << "Can't assign two different observers to "
           "base::sequence_manager:TaskQueue";
  }

  main_thread_only().task_queue_observer = observer;

  base::internal::CheckedAutoLock lock(any_thread_lock_);
  any_thread_.task_queue_observer = observer;
}

void TaskQueueImpl::UpdateDelayedWakeUp(LazyNow* lazy_now) {
  return UpdateDelayedWakeUpImpl(lazy_now, GetNextScheduledWakeUpImpl());
}

void TaskQueueImpl::UpdateDelayedWakeUpImpl(
    LazyNow* lazy_now,
    absl::optional<DelayedWakeUp> wake_up) {
  if (main_thread_only().scheduled_wake_up == wake_up)
    return;
  main_thread_only().scheduled_wake_up = wake_up;

  if (wake_up && main_thread_only().task_queue_observer &&
      !HasPendingImmediateWork()) {
    main_thread_only().task_queue_observer->OnQueueNextWakeUpChanged(
        wake_up->time);
  }

  main_thread_only().time_domain->SetNextWakeUpForQueue(this, wake_up,
                                                        lazy_now);
}

void TaskQueueImpl::SetDelayedWakeUpForTesting(
    absl::optional<DelayedWakeUp> wake_up) {
  LazyNow lazy_now = main_thread_only().time_domain->CreateLazyNow();
  UpdateDelayedWakeUpImpl(&lazy_now, wake_up);
}

bool TaskQueueImpl::HasPendingImmediateWork() {
  // Any work queue tasks count as immediate work.
  if (!main_thread_only().delayed_work_queue->Empty() ||
      !main_thread_only().immediate_work_queue->Empty()) {
    return true;
  }

  // Finally tasks on |immediate_incoming_queue| count as immediate work.
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  return !any_thread_.immediate_incoming_queue.empty();
}

bool TaskQueueImpl::HasPendingImmediateWorkLocked() {
  return !main_thread_only().delayed_work_queue->Empty() ||
         !main_thread_only().immediate_work_queue->Empty() ||
         !any_thread_.immediate_incoming_queue.empty();
}

void TaskQueueImpl::SetOnTaskStartedHandler(
    TaskQueueImpl::OnTaskStartedHandler handler) {
  DCHECK(should_notify_observers_ || handler.is_null());
  main_thread_only().on_task_started_handler = std::move(handler);
}

void TaskQueueImpl::OnTaskStarted(const Task& task,
                                  const TaskQueue::TaskTiming& task_timing) {
  if (!main_thread_only().on_task_started_handler.is_null())
    main_thread_only().on_task_started_handler.Run(task, task_timing);
}

void TaskQueueImpl::SetOnTaskCompletedHandler(
    TaskQueueImpl::OnTaskCompletedHandler handler) {
  DCHECK(should_notify_observers_ || handler.is_null());
  main_thread_only().on_task_completed_handler = std::move(handler);
}

void TaskQueueImpl::OnTaskCompleted(const Task& task,
                                    TaskQueue::TaskTiming* task_timing,
                                    LazyNow* lazy_now) {
  if (!main_thread_only().on_task_completed_handler.is_null()) {
    main_thread_only().on_task_completed_handler.Run(task, task_timing,
                                                     lazy_now);
  }
}

bool TaskQueueImpl::RequiresTaskTiming() const {
  return !main_thread_only().on_task_started_handler.is_null() ||
         !main_thread_only().on_task_completed_handler.is_null();
}

void TaskQueueImpl::SetOnTaskPostedHandler(OnTaskPostedHandler handler) {
  DCHECK(should_notify_observers_ || handler.is_null());
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  any_thread_.on_task_posted_handler = std::move(handler);
}

bool TaskQueueImpl::IsUnregistered() const {
  base::internal::CheckedAutoLock lock(any_thread_lock_);
  return any_thread_.unregistered;
}

WeakPtr<SequenceManagerImpl> TaskQueueImpl::GetSequenceManagerWeakPtr() {
  return sequence_manager_->GetWeakPtr();
}

void TaskQueueImpl::ActivateDelayedFenceIfNeeded(TimeTicks now) {
  if (!main_thread_only().delayed_fence)
    return;
  if (main_thread_only().delayed_fence.value() > now)
    return;
  InsertFence(TaskQueue::InsertFencePosition::kNow);
  main_thread_only().delayed_fence = absl::nullopt;
}

void TaskQueueImpl::MaybeReportIpcTaskQueuedFromMainThread(
    Task* pending_task,
    const char* task_queue_name) {
  if (!pending_task->ipc_hash)
    return;

  // It's possible that tracing was just enabled and no disabled time has been
  // stored. In that case, skip emitting the event.
  if (!main_thread_only().disabled_time)
    return;

  bool tracing_enabled = false;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("lifecycles"),
                                     &tracing_enabled);
  if (!tracing_enabled)
    return;

  if (main_thread_only().is_enabled ||
      !main_thread_only().should_report_posted_tasks_when_disabled) {
    return;
  }

  base::TimeDelta time_since_disabled =
      main_thread_only().time_domain->Now() -
      main_thread_only().disabled_time.value();

  ReportIpcTaskQueued(pending_task, task_queue_name, time_since_disabled);
}

bool TaskQueueImpl::ShouldReportIpcTaskQueuedFromAnyThreadLocked(
    base::TimeDelta* time_since_disabled) {
  // It's possible that tracing was just enabled and no disabled time has been
  // stored. In that case, skip emitting the event.
  if (!any_thread_.tracing_only.disabled_time)
    return false;

  if (any_thread_.tracing_only.is_enabled ||
      any_thread_.tracing_only.should_report_posted_tasks_when_disabled) {
    return false;
  }

  *time_since_disabled = any_thread_.time_domain->Now() -
                         any_thread_.tracing_only.disabled_time.value();
  return true;
}

void TaskQueueImpl::MaybeReportIpcTaskQueuedFromAnyThreadLocked(
    Task* pending_task,
    const char* task_queue_name) {
  if (!pending_task->ipc_hash)
    return;

  bool tracing_enabled = false;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("lifecycles"),
                                     &tracing_enabled);
  if (!tracing_enabled)
    return;

  base::TimeDelta time_since_disabled;
  if (ShouldReportIpcTaskQueuedFromAnyThreadLocked(&time_since_disabled))
    ReportIpcTaskQueued(pending_task, task_queue_name, time_since_disabled);
}

void TaskQueueImpl::MaybeReportIpcTaskQueuedFromAnyThreadUnlocked(
    Task* pending_task,
    const char* task_queue_name) {
  if (!pending_task->ipc_hash)
    return;

  bool tracing_enabled = false;
  TRACE_EVENT_CATEGORY_GROUP_ENABLED(TRACE_DISABLED_BY_DEFAULT("lifecycles"),
                                     &tracing_enabled);
  if (!tracing_enabled)
    return;

  base::TimeDelta time_since_disabled;
  bool should_report = false;
  {
    base::internal::CheckedAutoLock lock(any_thread_lock_);
    should_report =
        ShouldReportIpcTaskQueuedFromAnyThreadLocked(&time_since_disabled);
  }

  ReportIpcTaskQueued(pending_task, task_queue_name, time_since_disabled);
}

void TaskQueueImpl::ReportIpcTaskQueued(
    Task* pending_task,
    const char* task_queue_name,
    const base::TimeDelta& time_since_disabled) {
  TRACE_EVENT_INSTANT(
      TRACE_DISABLED_BY_DEFAULT("lifecycles"), "task_posted_to_disabled_queue",
      [&](perfetto::EventContext ctx) {
        auto* proto = ctx.event<perfetto::protos::pbzero::ChromeTrackEvent>()
                          ->set_chrome_task_posted_to_disabled_queue();
        proto->set_task_queue_name(task_queue_name);
        proto->set_time_since_disabled_ms(time_since_disabled.InMilliseconds());
        proto->set_ipc_hash(pending_task->ipc_hash);
        proto->set_source_location_iid(
            base::trace_event::InternedSourceLocation::Get(
                &ctx, base::trace_event::TraceSourceLocation(
                          pending_task->posted_from)));
      });
}

void TaskQueueImpl::OnQueueUnblocked() {
  DCHECK(IsQueueEnabled());
  DCHECK(!BlockedByFence());

  main_thread_only().enqueue_order_at_which_we_became_unblocked =
      sequence_manager_->GetNextSequenceNumber();

  static_assert(TaskQueue::QueuePriority::kLowPriority >
                    TaskQueue::QueuePriority::kNormalPriority,
                "Priorities are not ordered as expected");
  if (GetQueuePriority() <= TaskQueue::QueuePriority::kNormalPriority) {
    // We are kNormalPriority or more important so update
    // |enqueue_order_at_which_we_became_unblocked_with_normal_priority|.
    main_thread_only()
        .enqueue_order_at_which_we_became_unblocked_with_normal_priority =
        main_thread_only().enqueue_order_at_which_we_became_unblocked;
  }
}

TaskQueueImpl::DelayedIncomingQueue::DelayedIncomingQueue() = default;
TaskQueueImpl::DelayedIncomingQueue::~DelayedIncomingQueue() = default;

void TaskQueueImpl::DelayedIncomingQueue::push(Task&& task) {
  if (task.is_high_res)
    pending_high_res_tasks_++;
  queue_.push(std::move(task));
}

void TaskQueueImpl::DelayedIncomingQueue::pop() {
  DCHECK(!empty());
  if (top().is_high_res) {
    pending_high_res_tasks_--;
    DCHECK_GE(pending_high_res_tasks_, 0);
  }
  queue_.pop();
}

void TaskQueueImpl::DelayedIncomingQueue::swap(DelayedIncomingQueue* rhs) {
  std::swap(pending_high_res_tasks_, rhs->pending_high_res_tasks_);
  std::swap(queue_, rhs->queue_);
}

void TaskQueueImpl::DelayedIncomingQueue::SweepCancelledTasks(
    SequenceManagerImpl* sequence_manager) {
  pending_high_res_tasks_ -= queue_.SweepCancelledTasks(sequence_manager);
}

size_t TaskQueueImpl::DelayedIncomingQueue::PQueue::SweepCancelledTasks(
    SequenceManagerImpl* sequence_manager) {
  // Under the hood a std::priority_queue is a heap implemented top of a
  // std::vector. We poke at that vector directly here to filter out canceled
  // tasks in place.
  size_t num_high_res_tasks_swept = 0u;
  auto keep_task = [sequence_manager,
                    &num_high_res_tasks_swept](const Task& task) {
    // TODO(crbug.com/1155905): Remove after figuring out the cause of the
    // crash.
    sequence_manager->RecordCrashKeys(task);
    if (!task.task.IsCancelled())
      return true;
    if (task.is_high_res)
      num_high_res_tasks_swept++;
    return false;
  };

  // Because task destructors could have a side-effect of posting new tasks, we
  // move all the cancelled tasks into a temporary container before deleting
  // them. This is to avoid |c| from changing while c.erase() is running.
  auto delete_start = std::stable_partition(c.begin(), c.end(), keep_task);
  std::vector<Task> tasks_to_delete;
  std::move(delete_start, c.end(), std::back_inserter(tasks_to_delete));
  c.erase(delete_start, c.end());

  // stable_partition ensures order was not changed if there was nothing to
  // delete.
  if (!tasks_to_delete.empty()) {
    ranges::make_heap(c, comp);
    tasks_to_delete.clear();
  }
  return num_high_res_tasks_swept;
}

Value TaskQueueImpl::DelayedIncomingQueue::AsValue(TimeTicks now) const {
  return queue_.AsValue(now);
}

Value TaskQueueImpl::DelayedIncomingQueue::PQueue::AsValue(
    TimeTicks now) const {
  Value state(Value::Type::LIST);
  for (const Task& task : c)
    state.Append(TaskAsValue(task, now));
  return state;
}

}  // namespace internal
}  // namespace sequence_manager
}  // namespace base
