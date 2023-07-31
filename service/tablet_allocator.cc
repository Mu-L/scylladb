/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "locator/tablets.hh"
#include "replica/tablets.hh"
#include "locator/tablet_replication_strategy.hh"
#include "replica/database.hh"
#include "service/migration_manager.hh"
#include "service/tablet_allocator.hh"
#include "utils/stall_free.hh"
#include "locator/load_sketch.hh"
#include "utils/div_ceil.hh"

using namespace locator;
using namespace replica;

namespace service {

seastar::logger lblogger("load_balancer");

/// The algorithm aims to equalize tablet count on each shard.
/// This goal is based on the assumption that every shard has similar processing power and space capacity,
/// and that each tablet has equal consumption of those resources. So by equalizing tablet count per shard we
/// equalize resource utilization.
///
/// The algorithm produces a migration plan which is a set of instructions about which tablets to move
/// where. The plan is a small increment, not a complete plan. To achieve balance, the algorithm should
/// be invoked iteratively until an empty plan is returned.
///
/// The algorithm keeps track of load at two levels, per node and per shard. The reason for this is that
/// we want to equalize the per-node score first, by moving tablets across nodes. Tablets are moved away
/// from the most loaded node first. We also track load per shard, so that we move tablets from the most
/// loaded shard on a given node first.
///
/// The metric for node load is (number of tablets / shard count) which is the average
/// per-shard load. If we achieve balance according to this metric, and then rebalance the nodes internally,
/// we will achieve global balance on all shards in the cluster.
///
/// The reason why we focus on nodes first before rebalancing them internally is that this results
/// in less tablet movements than looking at shards only.
///
/// It would be still beneficial to rebalance tablet-receiving nodes internally before moving tablets
/// to them so that we can distribute load equally without overloading shards which are out of balance,
/// but this is not implemented yet.
///
/// The outline of the algorithm is as follows:
///
///   1. Determine the set of nodes whose load should be balanced.
///   2. Pick the least-loaded node (target)
///   3. Keep moving tablets to the target until balance is achieved with the highest-loaded node,
///              or we hit a limit for plan size:
///      3.1. Pick the most-loaded node (source)
///      3.2. Pick the most-loaded shard on the source
///      3.3. Pick one of the candidate tablets on the source shard
///      3.4. Evaluate collocation constraints for tablet replicas, if they pass:
///           3.4.1 Pick the least-loaded shard on the target
///           3.4.2 Generate a migration for the candidate tablet from the source shard to the target shard
///
/// Even though the algorithm focuses on a single target, the fact the the produced plan is just an increment
/// means that many under-loaded nodes can be driven forward to balance concurrently because the load balancer
/// will alternate between them across make_plan() calls.
///
/// The cost of make_plan() is relatively heavy in terms of preparing data structures, so the current
/// implementation is not efficient if the scheduler would like to call make_plan() multiple times
/// to parallelize execution. This will be addressed in the future by keeping the data structures
/// valid across calls and only recalculating them when starting a new round with a new token metadata version.
///
class load_balancer {
    using global_shard_id = tablet_replica;
    using shard_id = seastar::shard_id;

    // Represents metric for per-node load which we want to equalize between nodes.
    // It's an average per-shard load in terms of tablet count.
    using load_type = double;

    struct shard_load {
        size_t tablet_count;

        // Tablets which still have a replica on this shard which are candidates for migrating away from this shard.
        std::unordered_set<global_tablet_id> candidates;

        future<> clear_gently() {
            return utils::clear_gently(candidates);
        }
    };

    struct node_load {
        host_id node;
        uint64_t shard_count = 0;
        uint64_t tablet_count = 0;

        // The average shard load on this node.
        load_type avg_load = 0;

        std::vector<shard_id> shards_by_load; // heap which tracks most-loaded shards using shards_by_load_cmp().
        std::vector<shard_load> shards; // Indexed by shard_id to which a given shard_load corresponds.

        // Call when tablet_count changes.
        void update() {
            avg_load = get_avg_load(tablet_count);
        }

        load_type get_avg_load(uint64_t tablets) const {
            return double(tablets) / shard_count;
        }

        auto shards_by_load_cmp() {
            return [this] (const auto& a, const auto& b) {
                return shards[a].tablet_count < shards[b].tablet_count;
            };
        }

        future<> clear_gently() {
            return utils::clear_gently(shards);
        }
    };

    token_metadata_ptr _tm;
public:
    load_balancer(token_metadata_ptr tm)
        : _tm(std::move(tm)) {
    }

    future<migration_plan> make_plan() {
        const locator::topology& topo = _tm->get_topology();
        migration_plan plan;

        // Prepare plans for each DC separately and combine them to be executed in parallel.
        for (auto&& dc : topo.get_datacenters()) {
            auto dc_plan = co_await make_plan(dc);
            lblogger.info("Prepared {} migrations in DC {}", dc_plan.size(), dc);
            std::move(dc_plan.begin(), dc_plan.end(), std::back_inserter(plan));
        }

        lblogger.info("Prepared {} migrations", plan.size());
        co_return std::move(plan);
    }

    future<migration_plan> make_plan(sstring dc) {
        lblogger.info("Examining DC {}", dc);

        const locator::topology& topo = _tm->get_topology();

        // Select subset of nodes to balance.

        std::unordered_map<host_id, node_load> nodes;
        topo.for_each_node([&] (const locator::node* node_ptr) {
            if (node_ptr->get_state() == locator::node::state::normal && node_ptr->dc_rack().dc == dc) {
                node_load& load = nodes[node_ptr->host_id()];
                load.shard_count = node_ptr->get_shard_count();
                load.shards.resize(load.shard_count);
                if (!load.shard_count) {
                    throw std::runtime_error(format("Shard count of {} not found in topology", node_ptr->host_id()));
                }
            }
        });

        // Compute tablet load on nodes.

        for (auto&& [table, tmap] : _tm->tablets().all_tables()) {
            co_await tmap.for_each_tablet([&, table = table] (tablet_id tid, const tablet_info& ti) {
                for (auto&& replica : ti.replicas) {
                    if (nodes.contains(replica.host)) {
                        nodes[replica.host].tablet_count += 1;
                        // This invariant is assumed later.
                        if (replica.shard >= nodes[replica.host].shard_count) {
                            auto gtid = global_tablet_id{table, tid};
                            on_internal_error(lblogger, format("Tablet {} replica {} targets non-existent shard", gtid, replica));
                        }
                    }
                }
            });
        }

        // Compute load imbalance.

        load_type max_load = 0;
        load_type min_load = 0;
        std::optional<host_id> min_load_node = std::nullopt;
        for (auto&& [host, load] : nodes) {
            load.update();
            if (!min_load_node || load.avg_load < min_load) {
                min_load = load.avg_load;
                min_load_node = host;
            }
            if (load.avg_load > max_load) {
                max_load = load.avg_load;
            }
        }

        if (max_load == min_load) {
            // load is balanced.
            // TODO: Evaluate and fix intra-node balance.
            co_return migration_plan();
        }

        for (auto&& [host, load] : nodes) {
            lblogger.info("Node {}: rack={} avg_load={}, tablets={}, shards={}",
                          host, topo.find_node(host)->dc_rack().rack, load.avg_load, load.tablet_count, load.shard_count);
        }
        lblogger.info("target node: {}, avg_load: {}, max: {}", *min_load_node, min_load, max_load);
        auto target = *min_load_node;

        // We want to saturate the target node so we migrate several tablets in parallel, one for each shard
        // on the target node. This assumes that the target node is well-balanced and that tablet migrations
        // complete at the same time. Both assumptions are not generally true in practice, which we currently ignore.
        // If target node is not balanced across shards, we will overload some shards.
        // If tablets are not balanced in size, throughput will suffer because some shards will be idle sooner than others.
        //
        // FIXME: To handle the above, we should (1) rebalance the target node
        // before migrating tablets from other nodes. If shards are balanced on the target node, the balancer
        // will naturally distribute tablets to different shards. Also, (2) we should change this algorithm
        // to be a generator for migrations and have a scheduler in the execution layer which pulls migrations
        // from this algorithm, batches them and decides how many to execute.
        //
        // The scheduler decides in which order to execute the plan based on current activity in the system.
        // We cannot just ask the planner for the next migration and stop when we hit overload on some shard,
        // because that can lead to underutilization of the cluster. Just because the next migration is blocked
        // by the target shard being busy doesn't mean we could not proceed with migrations for other shards
        // which would be produced by the planner subsequently.

        auto target_node = topo.find_node(target);
        auto batch_size = target_node->get_shard_count();

        // Compute per-shard load and candidate tablets.

        for (auto&& [table, tmap] : _tm->tablets().all_tables()) {
            if (!tmap.transitions().empty()) {
                // FIXME: The algorithm doesn't support balancing with active transitions yet. They must finish first.
                lblogger.warn("Pending transitions active.");
                co_return migration_plan();
            }

            co_await tmap.for_each_tablet([&, table = table] (tablet_id tid, const tablet_info& ti) {
                for (auto&& replica : ti.replicas) {
                    if (!nodes.contains(replica.host)) {
                        continue;
                    }
                    auto& node_load_info = nodes[replica.host];
                    auto&& shard_load_info = node_load_info.shards[replica.shard];
                    if (shard_load_info.tablet_count == 0) {
                        node_load_info.shards_by_load.push_back(replica.shard);
                    }
                    shard_load_info.tablet_count += 1;
                    shard_load_info.candidates.emplace(global_tablet_id{table, tid});
                }
            });
        }

        // Prepare candidate nodes and shards for heap-based balancing.

        // heap which tracks most-loaded nodes in terms of avg_load.
        std::vector<host_id> nodes_by_load;
        nodes_by_load.reserve(nodes.size());
        auto nodes_cmp = [&] (const host_id& a, const host_id& b) {
            return nodes[a].avg_load < nodes[b].avg_load;
        };

        for (auto&& [host, node_load] : nodes) {
            if (lblogger.is_enabled(seastar::log_level::debug)) {
                shard_id shard = 0;
                for (auto&& shard_load : node_load.shards) {
                    lblogger.debug("shard {}: all tablets: {}, candidates: {}", tablet_replica{host, shard},
                                   shard_load.tablet_count, shard_load.candidates.size());
                    shard++;
                }
            }

            nodes_by_load.push_back(host);
            std::make_heap(node_load.shards_by_load.begin(), node_load.shards_by_load.end(), node_load.shards_by_load_cmp());
        }

        std::make_heap(nodes_by_load.begin(), nodes_by_load.end(), nodes_cmp);

        locator::load_sketch target_load(_tm);
        co_await target_load.populate(target);
        migration_plan plan;
        const tablet_metadata& tmeta = _tm->tablets();
        load_type max_off_candidate_load = 0; // max load among nodes which ran out of candidates.
        auto& target_info = nodes[target];
        while (plan.size() < batch_size && !nodes_by_load.empty()) {
            co_await coroutine::maybe_yield();

            std::pop_heap(nodes_by_load.begin(), nodes_by_load.end(), nodes_cmp);
            auto src_host = nodes_by_load.back();
            auto& src_node_info = nodes[src_host];

            // Check if all nodes reached the same avg_load. There are three sets of nodes: target, candidates (nodes_by_load)
            // and off-candidates (removed from nodes_by_load). At any time, the avg_load for target is not greater than
            // that of any candidate, and avg_load of any candidate is not greater than that of any in the off-candidates set.
            // This is ensured by the fact that we remove candidates in the order of avg_load from the heap, and
            // because we prevent load inversion between candidate and target in the next check.
            // So the max avg_load of candidates is that of the current src_node_info, and max avg_load of off-candidates
            // is tracked in max_off_candidate_load. If max_off_candidate_load is equal to target's avg_load,
            // it means that all nodes have equal avg_load. We take the maximum with the current candidate in src_node_info
            // to handle the case of off-candidates being empty. In that case, max_off_candidate_load is 0.
            if (std::max(max_off_candidate_load, src_node_info.avg_load) == target_info.avg_load) {
                lblogger.debug("Balance achieved.");
                break;
            }

            // If balance is not achieved, still consider migrating from candidate nodes which have higher load than the target.
            // max_off_candidate_load may be higher than the load of current candidate.
            if (src_node_info.avg_load <= target_info.avg_load) {
                lblogger.debug("No more candidate nodes.");
                lblogger.debug("No more candidate nodes. Next candidate is {} with avg_load={}, target's avg_load={}",
                               src_host, src_node_info.avg_load, target_info.avg_load);
                break;
            }

            // Prevent load inversion which can lead to oscillations.
            if (src_node_info.get_avg_load(nodes[src_host].tablet_count - 1) <
                    target_info.get_avg_load(target_info.tablet_count + 1)) {
                lblogger.debug("No more candidate nodes, load would be inverted. Next candidate is {} with avg_load={}, target's avg_load={}",
                               src_host, src_node_info.avg_load, target_info.avg_load);
                break;
            }

            if (src_node_info.shards_by_load.empty()) {
                lblogger.debug("candidate node {} ran out of candidate shards with {} tablets remaining.",
                               src_host, src_node_info.tablet_count);
                max_off_candidate_load = std::max(max_off_candidate_load, src_node_info.avg_load);
                nodes_by_load.pop_back();
                continue;
            }
            auto push_back_node_candidate = seastar::defer([&] {
                std::push_heap(nodes_by_load.begin(), nodes_by_load.end(), nodes_cmp);
            });

            std::pop_heap(src_node_info.shards_by_load.begin(), src_node_info.shards_by_load.end(), src_node_info.shards_by_load_cmp());
            auto src_shard = src_node_info.shards_by_load.back();
            auto src = tablet_replica{src_host, src_shard};
            auto&& src_shard_info = src_node_info.shards[src_shard];
            if (src_shard_info.candidates.empty()) {
                lblogger.debug("shard {} ran out of candidates with {} tablets remaining.", src, src_shard_info.tablet_count);
                src_node_info.shards_by_load.pop_back();
                continue;
            }
            auto push_back_shard_candidate = seastar::defer([&] {
                std::push_heap(src_node_info.shards_by_load.begin(), src_node_info.shards_by_load.end(), src_node_info.shards_by_load_cmp());
            });

            auto source_tablet = *src_shard_info.candidates.begin();
            src_shard_info.candidates.erase(source_tablet);

            // Check replication strategy constraints.

            auto same_rack = target_node->dc_rack().rack == topo.get_node(src.host).dc_rack().rack;
            std::unordered_map<sstring, int> rack_load; // Will be built if !same_rack
            bool has_replica_on_target = false;
            auto& tmap = tmeta.get_tablet_map(source_tablet.table);
            for (auto&& r : tmap.get_tablet_info(source_tablet.tablet).replicas) {
                if (r.host == target) {
                    has_replica_on_target = true;
                    break;
                }
                if (!same_rack) {
                    const locator::node& node = topo.get_node(r.host);
                    if (node.dc_rack().dc == dc) {
                        rack_load[node.dc_rack().rack] += 1;
                    }
                }
            }

            if (has_replica_on_target) {
                lblogger.debug("candidate tablet {} skipped because it has a replica on target node", source_tablet);
                continue;
            }

            // Make sure we don't increase level of duplication of racks in the replica list.
            if (!same_rack) {
                auto max_rack_load = std::max_element(rack_load.begin(), rack_load.end(),
                                                 [] (auto& a, auto& b) { return a.second < b.second; })->second;
                auto new_rack_load = rack_load[target_node->dc_rack().rack] + 1;
                if (new_rack_load > max_rack_load) {
                    lblogger.debug("candidate tablet {} skipped because it would increase load on rack {} to {}, max={}",
                                   source_tablet, target_node->dc_rack().rack, new_rack_load, max_rack_load);
                    continue;
                }
            }

            auto dst = global_shard_id {target, target_load.next_shard(target)};
            lblogger.debug("Select {} to move from {} to {}", source_tablet, src, dst);
            plan.push_back(tablet_migration_info {source_tablet, src, dst});

            target_info.tablet_count += 1;
            target_info.update();

            src_shard_info.tablet_count -= 1;
            if (src_shard_info.tablet_count == 0) {
                push_back_shard_candidate.cancel();
                src_node_info.shards_by_load.pop_back();
            }

            src_node_info.tablet_count -= 1;
            src_node_info.update();
            if (src_node_info.tablet_count == 0) {
                push_back_node_candidate.cancel();
                nodes_by_load.pop_back();
            }
        }

        if (plan.empty()) {
            // Due to replica collocation constraints, it may not be possible to balance the cluster evenly.
            // For example, if nodes have different number of shards. Nodes which have more shards will be
            // replicas for more tablets which rules out more candidates on other nodes with a higher per-shard load.
            //
            // Example:
            //
            //   node1: 1 shard
            //   node2: 1 shard
            //   node3: 7 shard
            //
            // If there are 7 tablets and RF=3, each node must have 1 tablet replica.
            // So node3 will have average load of 1, and node1 and node2 will have
            // average shard load of 7.
            lblogger.info("Not possible to achieve balance.");
        }

        co_await utils::clear_gently(nodes);
        co_return std::move(plan);
    }
};

future<migration_plan> balance_tablets(token_metadata_ptr tm) {
    load_balancer lb(tm);
    co_return co_await lb.make_plan();
}

class tablet_allocator_impl : public tablet_allocator::impl
                            , public service::migration_listener::empty_listener {
    service::migration_notifier& _migration_notifier;
    replica::database& _db;
    bool _stopped = false;
public:
    tablet_allocator_impl(service::migration_notifier& mn, replica::database& db)
            : _migration_notifier(mn)
            , _db(db) {
        _migration_notifier.register_listener(this);
    }

    tablet_allocator_impl(tablet_allocator_impl&&) = delete; // "this" captured.

    ~tablet_allocator_impl() {
        assert(_stopped);
    }

    future<> stop() {
        co_await _migration_notifier.unregister_listener(this);
        _stopped = true;
    }

    void on_before_create_column_family(const schema& s, std::vector<mutation>& muts, api::timestamp_type ts) override {
        keyspace& ks = _db.find_keyspace(s.ks_name());
        auto&& rs = ks.get_replication_strategy();
        if (auto&& tablet_rs = rs.maybe_as_tablet_aware()) {
            auto tm = _db.get_shared_token_metadata().get();
            auto map = tablet_rs->allocate_tablets_for_new_table(s.shared_from_this(), tm).get0();
            muts.emplace_back(tablet_map_to_mutation(map, s.id(), s.keypace_name(), s.cf_name(), ts).get0());
        }
    }

    void on_before_drop_column_family(const schema& s, std::vector<mutation>& muts, api::timestamp_type ts) override {
        keyspace& ks = _db.find_keyspace(s.ks_name());
        auto&& rs = ks.get_replication_strategy();
        std::vector<mutation> result;
        if (rs.uses_tablets()) {
            auto tm = _db.get_shared_token_metadata().get();
            muts.emplace_back(make_drop_tablet_map_mutation(s.keypace_name(), s.id(), ts));
        }
    }

    void on_before_drop_keyspace(const sstring& keyspace_name, std::vector<mutation>& muts, api::timestamp_type ts) override {
        keyspace& ks = _db.find_keyspace(keyspace_name);
        auto&& rs = ks.get_replication_strategy();
        if (rs.uses_tablets()) {
            auto tm = _db.get_shared_token_metadata().get();
            for (auto&& [name, s] : ks.metadata()->cf_meta_data()) {
                muts.emplace_back(make_drop_tablet_map_mutation(keyspace_name, s->id(), ts));
            }
        }
    }

    // FIXME: Handle materialized views.
};

tablet_allocator::tablet_allocator(service::migration_notifier& mn, replica::database& db)
    : _impl(std::make_unique<tablet_allocator_impl>(mn, db)) {
}

future<> tablet_allocator::stop() {
    return impl().stop();
}

tablet_allocator_impl& tablet_allocator::impl() {
    return static_cast<tablet_allocator_impl&>(*_impl);
}

}