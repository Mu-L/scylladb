#
# Copyright (C) 2024-present ScyllaDB
#
# SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.0
#
import pytest
import logging

from cassandra.cluster import ConsistencyLevel
from cassandra.policies import WhiteListRoundRobinPolicy
from cassandra.query import SimpleStatement

from test.pylib.manager_client import ManagerClient
from test.pylib.util import unique_name
from test.cluster.conftest import cluster_con
from test.cluster.util import create_new_test_keyspace


@pytest.mark.asyncio
async def test_zero_token_nodes_no_replication(manager: ManagerClient):
    """
    Test that zero-token nodes aren't replicas in all non-local replication strategies with and without tablets.
    """
    logging.info('Adding the first server')
    server_a = await manager.server_add(property_file={"dc": "dc1", "rack": "r1"})
    logging.info('Adding the second server as zero-token')
    server_b = await manager.server_add(config={'join_ring': False}, property_file={"dc": "dc1", "rack": "r2"})
    logging.info('Adding the third server')
    await manager.server_add(property_file={"dc": "dc1", "rack": "r3"})

    logging.info(f'Initiating connections to {server_a} and {server_b}')
    cql_a = cluster_con([server_a.ip_addr], 9042, False,
                        load_balancing_policy=WhiteListRoundRobinPolicy([server_a.ip_addr])).connect()
    cql_b = cluster_con([server_b.ip_addr], 9042, False,
                        load_balancing_policy=WhiteListRoundRobinPolicy([server_b.ip_addr])).connect()

    logging.info('Creating tables for each replication strategy and tablets combination')
    ks_names = list[str]()
    for replication_strategy in ['EverywhereStrategy', 'SimpleStrategy', 'NetworkTopologyStrategy']:
        for tablets_enabled in [True, False]:
            if tablets_enabled and replication_strategy != 'NetworkTopologyStrategy':
                continue

            ks_name = await create_new_test_keyspace(cql_b, f"""WITH replication =
                                    {{'class': '{replication_strategy}', 'replication_factor': 2}}
                                    AND tablets = {{ 'enabled': {str(tablets_enabled).lower()} }}""")
            ks_names.append(ks_name)
            await cql_b.run_async(f'CREATE TABLE {ks_name}.tbl (pk int PRIMARY KEY, v int)')
            for i in range(100):
                insert_query = f'INSERT INTO {ks_name}.tbl (pk, v) VALUES ({i}, {i})'
                if i % 2 == 0:
                    await cql_a.run_async(insert_query)
                else:
                    await cql_b.run_async(insert_query)  # Zero-token nodes should be able to coordinate requests.

    select_queries = {ks_name: SimpleStatement(f'SELECT * FROM {ks_name}.tbl', consistency_level=ConsistencyLevel.ALL)
                      for ks_name in ks_names}

    for ks_name in ks_names:
        result1 = [(row.pk, row.v) for row in await cql_b.run_async(select_queries[ks_name])]
        result1.sort()
        assert result1 == [(i, i) for i in range(100)]

    logging.info(f'Stopping {server_b}')
    await manager.server_stop_gracefully(server_b.server_id)

    # If server_b was a replica of some token range or tablet, reads with CL=ALL would fail.
    for ks_name in ks_names:
        result2 = [(row.pk, row.v) for row in await cql_a.run_async(select_queries[ks_name])]
        result2.sort()
        assert result2 == [(i, i) for i in range(100)]
