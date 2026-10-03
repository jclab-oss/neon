"""
Tests for local branches: computes that keep their changes on local disk, and only
read from the pageserver, at the LSN that the branch starts from.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

import psycopg2.errors
import pytest
from fixtures.common_types import Lsn, TimelineId
from fixtures.neon_fixtures import wait_for_last_flush_lsn
from fixtures.pg_version import PgVersion

if TYPE_CHECKING:
    from fixtures.neon_fixtures import Endpoint, NeonEnv, NeonEnvBuilder


def create_anchor_branch(env: NeonEnv, name: str, ancestor: Endpoint) -> tuple[TimelineId, Lsn]:
    """
    Create a branch for a local branch to start from, at the end of 'ancestor'.
    """
    lsn = wait_for_last_flush_lsn(
        env,
        ancestor,
        env.initial_tenant,
        env.initial_timeline,
        auth_token=env.auth_keys.generate_pageserver_token(),
    )
    timeline_id = env.create_branch(name, ancestor_branch_name="main", ancestor_start_lsn=lsn)
    return timeline_id, lsn


def table_contents(endpoint: Endpoint, table: str) -> list[tuple[int, str]]:
    return endpoint.safe_psql(f"SELECT id, val FROM {table} ORDER BY id")


@dataclass
class LocalMapStats:
    # Forks of relations that came from the pageserver and were modified locally
    remote_backed: int = 0
    # Blocks from the pageserver that were modified locally
    local_blocks: int = 0
    # Forks that were created locally
    local_only: int = 0
    # Relations that might exist on the pageserver, but were dropped locally
    tombstones: int = 0


def local_map_stats(endpoint: Endpoint) -> LocalMapStats:
    """
    Summarize the map files of a local branch, see pgxn/neon/local_branch.c.
    """
    stats = LocalMapStats()
    assert endpoint.pgdata_dir is not None
    for path in (endpoint.pgdata_dir / "base").glob("*/*.4294967295"):
        content = path.read_bytes()
        magic, version, flags, remote_nblocks = struct.unpack("<IIII", content[:16])
        assert (magic, version) == (0x4E4C4D31, 1)
        if not flags & 0x01:
            stats.tombstones += 1
        elif flags & 0x02:
            stats.remote_backed += 1
            bitmap = content[512 : 512 + (remote_nblocks + 7) // 8]
            stats.local_blocks += sum(bin(byte).count("1") for byte in bitmap)
        else:
            stats.local_only += 1
    return stats


def test_local_branch(neon_simple_env: NeonEnv):
    """
    A local branch sees the data of its ancestor, keeps its own changes, across
    buffer evictions and restarts, and doesn't change anything on the pageserver.
    """
    env = neon_simple_env

    main = env.endpoints.create_start("main")
    # Big enough to not fit in the endpoint's 1 MB of shared_buffers, so that pages
    # are evicted and read back.
    main.safe_psql("CREATE TABLE t (id int PRIMARY KEY, val text)")
    main.safe_psql("INSERT INTO t SELECT g, 'main' FROM generate_series(1, 20000) g")
    main.safe_psql("CREATE TABLE dropped (id int)")
    main.safe_psql("INSERT INTO dropped VALUES (1)")
    main.safe_psql("CREATE TABLE truncated AS SELECT g AS id FROM generate_series(1, 5000) g")
    anchor_timeline_id, branch_lsn = create_anchor_branch(env, "anchor", main)
    main_contents = table_contents(main, "t")

    local = env.endpoints.create_start("anchor", local_branch=True)
    assert table_contents(local, "t") == main_contents

    # Modify existing pages, add new ones, and create, drop and truncate relations.
    local.safe_psql("UPDATE t SET val = 'local' WHERE id % 3 = 0")
    local.safe_psql("DELETE FROM t WHERE id % 7 = 0")
    local.safe_psql("INSERT INTO t SELECT g, 'new' FROM generate_series(20001, 30000) g")
    local.safe_psql(
        "CREATE TABLE created AS SELECT g AS id, 'created' AS val FROM generate_series(1, 10000) g"
    )
    local.safe_psql("CREATE INDEX ON created (id)")
    local.safe_psql("DROP TABLE dropped")
    local.safe_psql("TRUNCATE truncated")
    local.safe_psql("VACUUM t")

    def check_local():
        expected = [
            (id, "local" if id % 3 == 0 else "main") for id in range(1, 20001) if id % 7 != 0
        ] + [(id, "new") for id in range(20001, 30001)]
        assert table_contents(local, "t") == expected
        assert local.safe_psql("SELECT count(*), sum(id) FROM created") == [(10000, 50005000)]
        assert local.safe_psql("SELECT count(*) FROM truncated") == [(0,)]
        assert local.safe_psql("SELECT to_regclass('dropped')") == [(None,)]
        assert local.safe_psql(
            "SET enable_seqscan = off; SELECT val FROM created WHERE id = 1234"
        ) == [("created",)]

    check_local()

    # The changes were written out to local storage, including pages that came
    # from the pageserver.
    local.safe_psql("CHECKPOINT")
    stats = local_map_stats(local)
    assert stats.remote_backed > 0
    assert stats.local_blocks > 0
    assert stats.local_only > 0
    assert stats.tombstones >= 2  # 'dropped', and the old relfilenode of 'truncated'

    # The changes survive a clean restart, and a crash.
    local.stop()
    local.start()
    check_local()

    local.safe_psql("INSERT INTO t VALUES (100000, 'after restart')")
    local.stop(mode="immediate")
    local.start()
    assert local.safe_psql("SELECT val FROM t WHERE id = 100000") == [("after restart",)]
    local.safe_psql("DELETE FROM t WHERE id = 100000")
    check_local()

    # Nothing changed in the ancestor, and the pageserver never saw the local
    # branch's WAL.
    assert table_contents(main, "t") == main_contents
    assert main.safe_psql("SELECT count(*) FROM dropped") == [(1,)]
    detail = env.pageserver.http_client().timeline_detail(env.initial_tenant, anchor_timeline_id)
    assert Lsn(detail["last_record_lsn"]) == branch_lsn


def test_local_branch_requires_quiet_timeline(neon_simple_env: NeonEnv):
    """
    Nothing may write to the timeline of a local branch.
    """
    env = neon_simple_env
    main = env.endpoints.create_start("main")
    create_anchor_branch(env, "anchor", main)

    env.endpoints.create_start("anchor", local_branch=True)
    with pytest.raises(Exception, match="nothing may write to the timeline of a local branch"):
        env.endpoints.create_start("anchor")


def test_local_branch_auth(neon_env_builder: NeonEnvBuilder):
    """
    A local branch works with a read-only storage token.
    """
    neon_env_builder.auth_enabled = True
    env = neon_env_builder.init_start()

    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE TABLE t AS SELECT g AS id FROM generate_series(1, 10000) g")
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True)
    local.safe_psql("INSERT INTO t SELECT g FROM generate_series(10001, 20000) g")
    assert local.safe_psql("SELECT count(*) FROM t") == [(20000,)]
    assert main.safe_psql("SELECT count(*) FROM t") == [(10000,)]


def test_local_branch_crash_recovery(neon_simple_env: NeonEnv):
    """
    Changes to pages that came from the pageserver survive a crash, whether they
    were written out before the crash or only exist in the WAL.
    """
    env = neon_simple_env
    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE TABLE t (id int PRIMARY KEY, val int)")
    main.safe_psql("INSERT INTO t SELECT g, 0 FROM generate_series(1, 50000) g")
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True)

    # Modify every page of the table: written out at the checkpoint
    local.safe_psql("UPDATE t SET val = 1")
    local.safe_psql("CHECKPOINT")
    # Modify them again: only in the WAL, and whatever got evicted
    local.safe_psql("UPDATE t SET val = val + 1 WHERE id % 2 = 0")
    local.safe_psql("INSERT INTO t SELECT g, 3 FROM generate_series(50001, 60000) g")

    for _ in range(2):
        local.stop(mode="immediate")
        local.start()
        assert local.safe_psql("SELECT val, count(*) FROM t GROUP BY val ORDER BY val") == [
            (1, 25000),
            (2, 25000),
            (3, 10000),
        ]

    # An uncommitted transaction is rolled back
    with local.cursor() as cur:
        cur.execute("BEGIN")
        cur.execute("UPDATE t SET val = 100")
        local.stop(mode="immediate")
    local.start()
    assert local.safe_psql("SELECT count(*) FROM t WHERE val = 100") == [(0,)]

    assert main.safe_psql("SELECT val, count(*) FROM t GROUP BY val") == [(0, 50000)]


def test_local_branch_ddl(neon_simple_env: NeonEnv):
    """
    Operations that create, rewrite, truncate or drop relations work in a local
    branch, also on relations that came from the pageserver.
    """
    env = neon_simple_env
    main = env.endpoints.create_start("main")
    main.safe_psql_many(
        [
            "CREATE TABLE rewritten (id int PRIMARY KEY, val text)",
            "INSERT INTO rewritten SELECT g, 'x' FROM generate_series(1, 10000) g",
            "CREATE TABLE shrunk (id int, val text)",
            "INSERT INTO shrunk SELECT g, 'x' FROM generate_series(1, 10000) g",
            "CREATE TABLE replaced (id int)",
            "INSERT INTO replaced VALUES (1)",
            "CREATE TABLE toasted (id int, val text)",
            "INSERT INTO toasted SELECT g, repeat(md5(g::text), 1000) FROM generate_series(1, 100) g",
            "CREATE SEQUENCE seq",
            "SELECT nextval('seq')",
        ]
    )
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True)
    local.safe_psql_many(
        [
            # Rewrite into a new relation, dropping the old one
            "UPDATE rewritten SET val = 'y' WHERE id <= 100",
            "VACUUM FULL rewritten",
            # Truncate a relation that came from the pageserver, then extend it again
            "DELETE FROM shrunk WHERE id > 100",
            "VACUUM shrunk",
            "INSERT INTO shrunk SELECT g, 'y' FROM generate_series(101, 200) g",
            # Drop a relation, and create a new one with the same name
            "DROP TABLE replaced",
            "CREATE TABLE replaced (id int)",
            "INSERT INTO replaced VALUES (2)",
            # TOAST
            "UPDATE toasted SET val = repeat(md5((id + 1)::text), 1000) WHERE id <= 50",
            "INSERT INTO toasted SELECT g, repeat(md5(g::text), 1000) FROM generate_series(101, 150) g",
            "SELECT nextval('seq')",
            # Relations that aren't permanent
            "CREATE UNLOGGED TABLE unlogged AS SELECT g AS id FROM generate_series(1, 1000) g",
        ]
    )

    def check():
        assert local.safe_psql("SELECT val, count(*) FROM rewritten GROUP BY val ORDER BY val") == [
            ("x", 9900),
            ("y", 100),
        ]
        assert local.safe_psql("SELECT val, count(*) FROM shrunk GROUP BY val ORDER BY val") == [
            ("x", 100),
            ("y", 100),
        ]
        assert local.safe_psql("SELECT * FROM replaced") == [(2,)]
        assert local.safe_psql(
            "SELECT count(*) FROM toasted WHERE val = repeat(md5((id + CASE WHEN id <= 50 THEN 1 ELSE 0 END)::text), 1000)"
        ) == [(150,)]

    check()
    assert local.safe_psql("SELECT count(*) FROM unlogged") == [(1000,)]
    with local.cursor() as cur:
        cur.execute("CREATE TEMP TABLE temp AS SELECT g AS id FROM generate_series(1, 1000) g")
        cur.execute("SELECT count(*) FROM temp")
        assert cur.fetchall() == [(1000,)]

    # A clean restart keeps unlogged tables, a crash empties them.
    local.stop()
    local.start()
    check()
    assert local.safe_psql("SELECT count(*) FROM unlogged") == [(1000,)]
    assert local.safe_psql("SELECT nextval('seq')") == [(3,)]
    local.stop(mode="immediate")
    local.start()
    check()
    assert local.safe_psql("SELECT count(*) FROM unlogged") == [(0,)]

    # The ancestor is unchanged
    assert main.safe_psql("SELECT count(*) FROM shrunk") == [(10000,)]
    assert main.safe_psql("SELECT * FROM replaced") == [(1,)]
    assert main.safe_psql("SELECT nextval('seq')") == [(2,)]


def test_local_branch_create_database(neon_simple_env: NeonEnv):
    env = neon_simple_env
    if env.pg_version == PgVersion.V14:
        pytest.skip("CREATE DATABASE is not supported in local branches on PostgreSQL 14")

    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE DATABASE source")
    main.safe_psql(
        "CREATE TABLE t AS SELECT g AS id FROM generate_series(1, 1000) g", dbname="source"
    )
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True)
    local.safe_psql("CREATE DATABASE copy TEMPLATE source")
    with pytest.raises(psycopg2.errors.FeatureNotSupported, match="FILE_COPY"):
        local.safe_psql("CREATE DATABASE copy2 TEMPLATE source STRATEGY file_copy")

    # The new database has a copy of the template, and is stored locally only
    local.safe_psql("INSERT INTO t VALUES (1001)", dbname="copy")
    assert local.safe_psql("SELECT count(*) FROM t", dbname="copy") == [(1001,)]
    assert local.safe_psql("SELECT count(*) FROM t", dbname="source") == [(1000,)]
    assert local.safe_psql("SELECT pg_database_size('copy') > 0") == [(True,)]

    local.stop(mode="immediate")
    local.start()
    assert local.safe_psql("SELECT count(*) FROM t", dbname="copy") == [(1001,)]
    local.safe_psql("DROP DATABASE copy")


def test_local_branch_siblings(neon_simple_env: NeonEnv):
    """
    Local branches from the same branch are independent of each other.
    """
    env = neon_simple_env
    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE TABLE t (id int PRIMARY KEY, val text)")
    main.safe_psql("INSERT INTO t SELECT g, 'main' FROM generate_series(1, 10000) g")
    create_anchor_branch(env, "anchor", main)

    first = env.endpoints.create_start("anchor", local_branch=True)
    second = env.endpoints.create_start("anchor", local_branch=True)
    first.safe_psql("UPDATE t SET val = 'first' WHERE id % 2 = 0")
    second.safe_psql("UPDATE t SET val = 'second' WHERE id % 3 = 0")

    assert first.safe_psql("SELECT val, count(*) FROM t GROUP BY val ORDER BY val") == [
        ("first", 5000),
        ("main", 5000),
    ]
    assert second.safe_psql("SELECT val, count(*) FROM t GROUP BY val ORDER BY val") == [
        ("main", 6667),
        ("second", 3333),
    ]


@pytest.mark.parametrize("grpc", [False, True])
def test_local_branch_sharded(neon_env_builder: NeonEnvBuilder, grpc: bool):
    """
    A local branch of a sharded tenant reads from all shards. With gRPC, compute_ctl
    takes the basebackup over gRPC.
    """
    env = neon_env_builder.init_start(
        initial_tenant_shard_count=2, initial_tenant_shard_stripe_size=8
    )

    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE TABLE t (id int PRIMARY KEY, val text)")
    main.safe_psql("INSERT INTO t SELECT g, 'main' FROM generate_series(1, 20000) g")
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True, grpc=grpc)
    local.safe_psql("UPDATE t SET val = 'local' WHERE id % 2 = 0")
    local.stop(mode="immediate")
    local.start()
    assert local.safe_psql("SELECT val, count(*) FROM t GROUP BY val ORDER BY val") == [
        ("local", 10000),
        ("main", 10000),
    ]
    assert main.safe_psql("SELECT val, count(*) FROM t GROUP BY val") == [("main", 20000)]


def test_local_branch_lost_writes(neon_simple_env: NeonEnv):
    """
    Pages from the pageserver that were written locally for the first time since
    the last checkpoint are restored by WAL replay, even if their data was lost
    in a crash, e.g. a power failure, while their bits in the map were not.
    """
    env = neon_simple_env
    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE TABLE t (id int PRIMARY KEY, val int)")
    main.safe_psql("INSERT INTO t SELECT g, 0 FROM generate_series(1, 50000) g")
    main.safe_psql("CREATE TABLE vacuumed (id int) WITH (autovacuum_enabled = off)")
    main.safe_psql("INSERT INTO vacuumed SELECT generate_series(1, 50000)")
    create_anchor_branch(env, "anchor", main)

    local = env.endpoints.create_start("anchor", local_branch=True)
    relfilenodes = [
        local.safe_psql(f"SELECT pg_relation_filepath('{table}')")[0][0]
        for table in ["t", "vacuumed"]
    ]
    local.safe_psql("CHECKPOINT")
    # Modify all pages of the table, which evicts most of them.
    local.safe_psql("UPDATE t SET val = 1")
    local.safe_psql("INSERT INTO t SELECT g, 2 FROM generate_series(50001, 60000) g")
    # Set hint bits and the all-visible flag, without WAL-logging the pages
    local.safe_psql("VACUUM vacuumed")
    assert local_map_stats(local).local_blocks > 0
    local.stop(mode="immediate")

    # Lose everything written to the tables' files since the checkpoint, but keep
    # the map files, as if they had been flushed to disk and the data hadn't.
    assert local.pgdata_dir is not None
    for relfilenode in relfilenodes:
        for fork in ["", "_fsm", "_vm"]:
            path = local.pgdata_dir / f"{relfilenode}{fork}"
            if path.exists():
                size = path.stat().st_size
                with open(path, "r+b") as f:
                    f.write(b"\0" * size)

    local.start()
    assert local.safe_psql("SELECT val, count(*) FROM t GROUP BY val ORDER BY val") == [
        (1, 50000),
        (2, 10000),
    ]
    assert local.safe_psql("SELECT count(*) FROM vacuumed") == [(50000,)]
    assert local.safe_psql("SET enable_seqscan = off; SELECT count(*) FROM t WHERE id > 0") == [
        (60000,)
    ]
