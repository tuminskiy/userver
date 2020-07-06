#include <storages/postgres/detail/cluster_impl.hpp>

#include <fmt/format.h>

#include <engine/async.hpp>

#include <storages/postgres/dsn.hpp>
#include <storages/postgres/exceptions.hpp>

namespace storages::postgres::detail {

namespace {

struct TryLockGuard {
  TryLockGuard(std::atomic_flag& lock) : lock_(lock) {
    lock_acquired_ = !lock_.test_and_set(std::memory_order_acq_rel);
  }

  ~TryLockGuard() {
    if (lock_acquired_) {
      lock_.clear(std::memory_order_release);
    }
  }

  bool LockAcquired() const { return lock_acquired_; }

 private:
  std::atomic_flag& lock_;
  bool lock_acquired_;
};

ClusterHostType Fallback(ClusterHostType ht) {
  switch (ht) {
    case ClusterHostType::kMaster:
      throw ClusterError("Cannot fallback from master");
    case ClusterHostType::kSyncSlave:
    case ClusterHostType::kSlave:
      return ClusterHostType::kMaster;
    case ClusterHostType::kNone:
    case ClusterHostType::kRoundRobin:
    case ClusterHostType::kNearest:
      throw ClusterError("Invalid ClusterHostType value for fallback " +
                         ToString(ht));
  }
}

size_t SelectDsnIndex(const QuorumCommitTopology::DsnIndices& indices,
                      ClusterHostTypeFlags flags,
                      std::atomic<uint32_t>& rr_host_idx) {
  UASSERT(!indices.empty());
  if (indices.empty()) {
    throw ClusterError("Cannot select host from an empty list");
  }

  const auto strategy_flags = flags & kClusterHostStrategyMask;
  LOG_TRACE() << "Applying " << strategy_flags << " strategy";

  size_t idx_pos = 0;
  if (!strategy_flags || strategy_flags == ClusterHostType::kRoundRobin) {
    if (indices.size() != 1) {
      idx_pos =
          rr_host_idx.fetch_add(1, std::memory_order_relaxed) % indices.size();
    }
  } else if (strategy_flags != ClusterHostType::kNearest) {
    throw LogicError(
        fmt::format("Invalid strategy requested: {}, ensure only one is used",
                    ToString(strategy_flags)));
  }
  return indices[idx_pos];
}

}  // namespace

ClusterImpl::ClusterImpl(DsnList dsns, engine::TaskProcessor& bg_task_processor,
                         const PoolSettings& pool_settings,
                         const ConnectionSettings& conn_settings,
                         const CommandControl& default_cmd_ctl,
                         const testsuite::PostgresControl& testsuite_pg_ctl,
                         const error_injection::Settings& ei_settings)
    : topology_(bg_task_processor, std::move(dsns), conn_settings,
                default_cmd_ctl, testsuite_pg_ctl, ei_settings),
      bg_task_processor_(bg_task_processor),
      rr_host_idx_(0) {
  const auto& dsn_list = topology_.GetDsnList();
  if (dsn_list.empty()) {
    throw ClusterError("Cannot create a cluster from an empty DSN list");
  }

  LOG_DEBUG() << "Starting pools initialization";
  host_pools_.reserve(dsn_list.size());
  for (const auto& dsn : dsn_list) {
    host_pools_.push_back(ConnectionPool::Create(
        dsn, bg_task_processor_, pool_settings, conn_settings, default_cmd_ctl,
        testsuite_pg_ctl, ei_settings));
  }
  LOG_DEBUG() << "Pools initialized";
}

ClusterImpl::~ClusterImpl() = default;

ClusterStatisticsPtr ClusterImpl::GetStatistics() const {
  auto cluster_stats = std::make_unique<ClusterStatistics>();
  const auto& dsns = topology_.GetDsnList();
  std::vector<int8_t> is_host_pool_seen(dsns.size(), 0);
  auto dsn_indices_by_type = topology_.GetDsnIndicesByType();

  UASSERT(host_pools_.size() == dsns.size());

  // TODO remove code duplication
  auto master_dsn_indices_it =
      dsn_indices_by_type->find(ClusterHostType::kMaster);
  if (master_dsn_indices_it != dsn_indices_by_type->end() &&
      !master_dsn_indices_it->second.empty()) {
    auto dsn_index = master_dsn_indices_it->second.front();
    UASSERT(dsn_index < dsns.size());
    cluster_stats->master.host_port = GetHostPort(dsns[dsn_index]);
    const auto& pool = host_pools_[dsn_index];
    cluster_stats->master.stats = pool->GetStatistics();
    is_host_pool_seen[dsn_index] = 1;
  }

  auto sync_slave_dsn_indices_it =
      dsn_indices_by_type->find(ClusterHostType::kSyncSlave);
  if (sync_slave_dsn_indices_it != dsn_indices_by_type->end() &&
      !sync_slave_dsn_indices_it->second.empty()) {
    auto dsn_index = sync_slave_dsn_indices_it->second.front();
    UASSERT(dsn_index < dsns.size());
    cluster_stats->sync_slave.host_port = GetHostPort(dsns[dsn_index]);
    UASSERT(dsn_index < host_pools_.size());
    const auto& pool = host_pools_[dsn_index];
    cluster_stats->sync_slave.stats = pool->GetStatistics();
    is_host_pool_seen[dsn_index] = 1;
  }

  auto slaves_dsn_indices_it =
      dsn_indices_by_type->find(ClusterHostType::kSlave);
  if (slaves_dsn_indices_it != dsn_indices_by_type->end() &&
      !slaves_dsn_indices_it->second.empty()) {
    cluster_stats->slaves.reserve(slaves_dsn_indices_it->second.size());
    for (auto dsn_index : slaves_dsn_indices_it->second) {
      if (is_host_pool_seen[dsn_index]) continue;

      InstanceStatsDescriptor slave_desc;
      UASSERT(dsn_index < dsns.size());
      slave_desc.host_port = GetHostPort(dsns[dsn_index]);
      UASSERT(dsn_index < host_pools_.size());
      const auto& pool = host_pools_[dsn_index];
      slave_desc.stats = pool->GetStatistics();
      is_host_pool_seen[dsn_index] = 1;

      cluster_stats->slaves.push_back(std::move(slave_desc));
    }
  }
  for (size_t i = 0; i < is_host_pool_seen.size(); ++i) {
    if (is_host_pool_seen[i]) continue;

    InstanceStatsDescriptor desc;
    UASSERT(i < dsns.size());
    desc.host_port = GetHostPort(dsns[i]);
    UASSERT(i < host_pools_.size());
    const auto& pool = host_pools_[i];
    desc.stats = pool->GetStatistics();

    cluster_stats->unknown.push_back(std::move(desc));
  }
  return cluster_stats;
}

ClusterImpl::ConnectionPoolPtr ClusterImpl::FindPool(
    ClusterHostTypeFlags flags) {
  LOG_TRACE() << "Looking for pool: " << flags;

  size_t dsn_index = -1;
  const auto role_flags = flags & kClusterHostRolesMask;

  UASSERT_MSG(role_flags, "No roles specified");
  UASSERT_MSG(!(role_flags & ClusterHostType::kSyncSlave) ||
                  role_flags == ClusterHostType::kSyncSlave,
              "kSyncSlave cannot be combined with other roles");

  if ((role_flags & ClusterHostType::kMaster) &&
      (role_flags & ClusterHostType::kSlave)) {
    LOG_TRACE() << "Starting transaction on " << role_flags;
    auto alive_dsn_indices = topology_.GetAliveDsnIndices();
    if (alive_dsn_indices->empty()) {
      throw ClusterUnavailable("None of cluster hosts are available");
    }
    dsn_index = SelectDsnIndex(*alive_dsn_indices, flags, rr_host_idx_);
  } else {
    auto host_role = static_cast<ClusterHostType>(role_flags.GetValue());
    auto dsn_indices_by_type = topology_.GetDsnIndicesByType();
    auto dsn_indices_it = dsn_indices_by_type->find(host_role);
    while (host_role != ClusterHostType::kMaster &&
           (dsn_indices_it == dsn_indices_by_type->end() ||
            dsn_indices_it->second.empty())) {
      auto fb = Fallback(host_role);
      LOG_WARNING() << "There is no pool for " << host_role
                    << ", falling back to " << fb;
      host_role = fb;
      dsn_indices_it = dsn_indices_by_type->find(host_role);
    }

    if (dsn_indices_it == dsn_indices_by_type->end() ||
        dsn_indices_it->second.empty()) {
      throw ClusterUnavailable(
          fmt::format("Pool for {} (requested: {}) is not available",
                      ToString(host_role), ToString(role_flags)));
    }
    LOG_TRACE() << "Starting transaction on " << host_role;
    dsn_index = SelectDsnIndex(dsn_indices_it->second, flags, rr_host_idx_);
  }

  UASSERT(dsn_index < host_pools_.size());
  return host_pools_.at(dsn_index);
}

Transaction ClusterImpl::Begin(ClusterHostTypeFlags flags,
                               const TransactionOptions& options,
                               OptionalCommandControl cmd_ctl) {
  LOG_TRACE() << "Requested transaction on " << flags;
  const auto role_flags = flags & kClusterHostRolesMask;
  if (options.IsReadOnly()) {
    if (!role_flags) {
      flags |= ClusterHostType::kSlave;
    }
  } else {
    if (role_flags && !(role_flags & ClusterHostType::kMaster)) {
      throw ClusterUnavailable("Cannot start RW-transaction on a slave");
    }
    flags = ClusterHostType::kMaster | flags.Clear(kClusterHostRolesMask);
  }
  return FindPool(flags)->Begin(options, cmd_ctl);
}

NonTransaction ClusterImpl::Start(ClusterHostTypeFlags flags,
                                  OptionalCommandControl cmd_ctl) {
  if (!(flags & kClusterHostRolesMask)) {
    throw LogicError(
        "Host role must be specified for execution of a single statement");
  }
  LOG_TRACE() << "Requested single statement on " << flags;
  return FindPool(flags)->Start(cmd_ctl);
}

void ClusterImpl::SetDefaultCommandControl(CommandControl cmd_ctl,
                                           DefaultCommandControlSource source) {
  for (const auto& pool_ptr : host_pools_) {
    pool_ptr->SetDefaultCommandControl(cmd_ctl, source);
  }
}

CommandControl ClusterImpl::GetDefaultCommandControl() const {
  UASSERT(!host_pools_.empty());
  return host_pools_.front()->GetDefaultCommandControl();
}

}  // namespace storages::postgres::detail
