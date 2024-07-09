/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <mutex>
#include <thread>

#include "core/interface/lease_manager_interface.h"

namespace google::scp::core {
/**
 * @copydoc LeaseRefresherInterface
 *
 * Automatic lease refresher that employs an internal worker thread.
 */
class LeaseRefresher : public LeaseRefresherInterface,
                       public LeaseRefreshLivenessCheckInterface {
 public:
  LeaseRefresher(
      const LeasableLockId& leasable_lock_id,
      const std::shared_ptr<LeasableLockInterface>& leasable_lock,
      const std::shared_ptr<LeaseEventSinkInterface>& lease_event_sink);

  ExecutionResult Init() noexcept override;

  ExecutionResult Run() noexcept override;

  ExecutionResult Stop() noexcept override;

  /**
   * @brief Returns the current lease refresh mode.
   *
   * NOTE: Running time of this call will not be affected by what the refresher
   * is doing at that moment.
   *
   * @return LeaseRefreshMode
   */
  LeaseRefreshMode GetLeaseRefreshMode() const noexcept override;

  /**
   * @brief Sets the lease refresh mode.
   *
   * NOTE: If there is an ongoing lease refresh, this is blocked until the
   * refresh round is completed.
   *
   * @param lease_refresh_mode
   *
   * @return ExecutionResult
   */
  ExecutionResult SetLeaseRefreshMode(
      LeaseRefreshMode lease_refresh_mode) noexcept override;

  /**
   * @brief This allows Lease Refresh Enforcer to check liveness of this
   * refresher component.
   *
   * NOTE: Running time of this call will not be affected by what the refresher
   * is doing at that moment.
   *
   * @return std::chrono::nanoseconds
   */
  std::chrono::nanoseconds GetLastLeaseRefreshTimestamp()
      const noexcept override;

  /**
   * @brief Refreshes lease if needed by caller. LeaseRefresher employs a thread
   * internally to refresh lease periodically as well.
   */
  ExecutionResult PerformLeaseRefresh() noexcept override;

 protected:
  /**
   * @brief Lease refresh round
   */
  void LeaseRefreshRound();

  /**
   * @brief Lease refresh thread's function
   */
  void LeaseRefreshThreadFunction();

  /// @brief Leasable lock that is managed by this refresher.
  std::shared_ptr<LeasableLockInterface> leasable_lock_;
  /// @brief Sink of the lease transition events generated by this refresher.
  /// This is a weak_ptr to avoid ownership.
  std::weak_ptr<LeaseEventSinkInterface> lease_event_sink_;
  /// @brief The previous mode of lease refresher.
  std::atomic<LeaseRefreshMode> prev_lease_refresh_mode_;
  /// @brief The current mode of lease refresher.
  std::atomic<LeaseRefreshMode> lease_refresh_mode_;
  /// @brief Lease refresher thread
  std::unique_ptr<std::thread> lease_refresher_thread_;
  /// @brief Lease refresher mutex
  std::mutex lease_refresh_mutex_;
  /// @brief Last lease refresh timestamp
  std::atomic<std::chrono::nanoseconds> last_lease_refresh_timestamp_;
  /// @brief Is running?
  bool is_running_;
  /// @brief Is lease owner in last refresh.
  bool was_lease_owner_;
  /// @brief lock id
  LeasableLockId leasable_lock_id_;
  /// @brief Activity ID for the lifetime of the object
  core::common::Uuid object_activity_id_;
  /// @brief Previous lease transition type that is generated by the lease
  /// refresher.
  std::optional<LeaseTransitionType> last_lease_transition_;
};
}  // namespace google::scp::core
