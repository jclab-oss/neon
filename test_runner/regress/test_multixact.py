from __future__ import annotations

import pytest
from fixtures.log_helper import log
from fixtures.neon_fixtures import NeonEnv, NeonEnvBuilder, check_restored_datadir_content
from fixtures.pg_version import PgVersion
from fixtures.utils import query_scalar, skip_on_postgres

# See src/include/access/multixact.h and slru.h
MULTIXACT_OFFSETS_PER_PAGE = 8192 // 4
SLRU_PAGES_PER_SEGMENT = 32


#
# Test multixact state after branching
# Now this test is very minimalistic -
# it only checks next_multixact_id field in restored pg_control,
# since we don't have functions to check multixact internals.
# We do check that the datadir contents exported from the
# pageserver match what the running PostgreSQL produced. This
# is enough to verify that the WAL records are handled correctly
# in the pageserver.
#
def test_multixact(neon_simple_env: NeonEnv, test_output_dir):
    env = neon_simple_env
    endpoint = env.endpoints.create_start("main")

    cur = endpoint.connect().cursor()
    cur.execute(
        """
        CREATE TABLE t1(i int primary key, n_updated int);
        INSERT INTO t1 select g, 0 from generate_series(1, 50) g;
    """
    )

    next_multixact_id_old = query_scalar(
        cur, "SELECT next_multixact_id FROM pg_control_checkpoint()"
    )

    # Lock entries using parallel connections in a round-robin fashion.
    nclients = 20
    update_every = 97
    connections = []
    for _ in range(nclients):
        # Do not turn on autocommit. We want to hold the key-share locks.
        conn = endpoint.connect(autocommit=False)
        connections.append(conn)

    # On each iteration, we commit the previous transaction on a connection,
    # and issue another select. Each SELECT generates a new multixact that
    # includes the new XID, and the XIDs of all the other parallel transactions.
    # This generates enough traffic on both multixact offsets and members SLRUs
    # to cross page boundaries.
    for i in range(20000):
        conn = connections[i % nclients]
        conn.commit()

        # Perform some non-key UPDATEs too, to exercise different multixact
        # member statuses.
        if i % update_every == 0:
            conn.cursor().execute(f"update t1 set n_updated = n_updated + 1 where i = {i % 50}")
        else:
            conn.cursor().execute("select * from t1 for key share")

    # We have multixacts now. We can close the connections.
    for c in connections:
        c.close()

    # force wal flush
    cur.execute("checkpoint")

    cur.execute(
        "SELECT next_multixact_id, pg_current_wal_insert_lsn() FROM pg_control_checkpoint()"
    )
    res = cur.fetchone()
    assert res is not None
    next_multixact_id = res[0]
    lsn = res[1]

    # Ensure that we did lock some tuples
    assert int(next_multixact_id) > int(next_multixact_id_old)

    # Branch at this point
    env.create_branch("test_multixact_new", ancestor_branch_name="main", ancestor_start_lsn=lsn)
    endpoint_new = env.endpoints.create_start("test_multixact_new")

    next_multixact_id_new = endpoint_new.safe_psql(
        "SELECT next_multixact_id FROM pg_control_checkpoint()"
    )[0][0]

    # Check that we restored pg_controlfile correctly
    assert next_multixact_id_new == next_multixact_id

    # Check that we can restore the content of the datadir correctly
    check_restored_datadir_content(test_output_dir, env, endpoint)


#
# Since PostgreSQL 17.7, RecordNewMultiXact() also sets the offset of the next
# multixid, and GetMultiXactIdMembers() errors out if that offset is still zero
# ("MultiXact %u has invalid next offset"). The pageserver only stores each
# multixid's own offset, and fills in the next one's from the checkpoint when it
# serves the page. Check that a compute started from the pageserver sees that
# entry, both in the basebackup and when the segment is downloaded on demand, and
# when the next multixid is the first one on a new page.
#
@skip_on_postgres(PgVersion.V14, "the next multixid's offset is only set since 17.7")
@skip_on_postgres(PgVersion.V15, "the next multixid's offset is only set since 17.7")
@skip_on_postgres(PgVersion.V16, "the next multixid's offset is only set since 17.7")
@pytest.mark.parametrize("lazy_slru_download", [False, True])
@pytest.mark.parametrize("page_boundary", [False, True])
def test_multixact_next_offset(
    neon_env_builder: NeonEnvBuilder, lazy_slru_download: bool, page_boundary: bool
):
    env = neon_env_builder.init_start(
        initial_tenant_conf={"lazy_slru_download": lazy_slru_download}
    )
    endpoint = env.endpoints.create_start("main")

    endpoint.safe_psql("CREATE TABLE t(i int primary key)")
    endpoint.safe_psql("INSERT INTO t VALUES (1)")

    cur = endpoint.connect().cursor()
    conn1 = endpoint.connect(autocommit=False)
    conn2 = endpoint.connect(autocommit=False)

    def create_multixact() -> int:
        # Two transactions locking the same row make its xmax a new multixid
        conn1.cursor().execute("SELECT * FROM t FOR KEY SHARE")
        conn2.cursor().execute("SELECT * FROM t FOR KEY SHARE")
        mxid = int(query_scalar(cur, "SELECT xmax::text::bigint FROM t"))
        conn1.commit()
        conn2.commit()
        return mxid

    mxid = create_multixact()
    if page_boundary:
        # Create multixids until the next one is the first on a new page
        while (mxid + 1) % MULTIXACT_OFFSETS_PER_PAGE != 0:
            mxid = create_multixact()
    else:
        for _ in range(10):
            mxid = create_multixact()
    next_mxid = mxid + 1
    log.info(f"last multixid {mxid}, next multixid {next_mxid}")

    conn1.close()
    conn2.close()

    # Restart, so that the multixact offsets pages come from the pageserver
    endpoint.stop()
    endpoint.start()
    cur = endpoint.connect().cursor()

    cur.execute("SELECT next_multixact_id, next_multi_offset FROM pg_control_checkpoint()")
    row = cur.fetchone()
    assert row is not None
    assert int(row[0]) == next_mxid
    next_offset = int(row[1])
    assert next_offset > 0

    # Read the last multixid's members. That also downloads the segment, with
    # lazy_slru_download.
    assert query_scalar(cur, f"SELECT count(*) FROM pg_get_multixact_members('{mxid}')") == 2

    # The next multixid's entry must be set, to where the last multixid's members end
    segno = next_mxid // (MULTIXACT_OFFSETS_PER_PAGE * SLRU_PAGES_PER_SEGMENT)
    entry = next_mxid % (MULTIXACT_OFFSETS_PER_PAGE * SLRU_PAGES_PER_SEGMENT)
    raw = query_scalar(
        cur,
        f"SELECT pg_read_binary_file('pg_multixact/offsets/{segno:04X}', {entry * 4}, 4)",
    )
    assert int.from_bytes(bytes(raw), "little") == next_offset
