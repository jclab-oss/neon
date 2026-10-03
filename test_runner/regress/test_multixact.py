from __future__ import annotations

import shutil
import subprocess
import threading
import time
from typing import TYPE_CHECKING

import pytest
from fixtures.log_helper import log
from fixtures.neon_fixtures import (
    NeonEnv,
    NeonEnvBuilder,
    check_restored_datadir_content,
    wait_for_wal_insert_lsn,
)
from fixtures.pg_version import PgVersion
from fixtures.utils import query_scalar, skip_on_postgres

if TYPE_CHECKING:
    from pathlib import Path

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


#
# Multixids are assigned before their CREATE_ID record is WAL-logged, so with
# concurrent backends the records can arrive in a different order than the
# multixids were assigned. Since 17.7, reading a multixid's members needs the
# starting offset of the next multixid, which PostgreSQL sets when it creates the
# multixid, before the next one's record. Check that a replica started while a
# multixid is assigned but not yet logged can read the multixid before it.
#
# To get there deterministically, this pauses a backend in gdb right before it
# WAL-logs its multixid, so it needs gdb and permission to attach to postgres.
#
@skip_on_postgres(PgVersion.V14, "the next multixid's offset is only set since 17.7")
@skip_on_postgres(PgVersion.V15, "the next multixid's offset is only set since 17.7")
@skip_on_postgres(PgVersion.V16, "the next multixid's offset is only set since 17.7")
@pytest.mark.parametrize("lazy_slru_download", [False, True])
def test_multixact_out_of_order(
    neon_env_builder: NeonEnvBuilder, test_output_dir: Path, lazy_slru_download: bool
):
    if shutil.which("gdb") is None:
        pytest.skip("gdb is not available")

    env = neon_env_builder.init_start(
        initial_tenant_conf={"lazy_slru_download": lazy_slru_download}
    )
    primary = env.endpoints.create_start("main", config_lines=["autovacuum=off"])
    primary.safe_psql("CREATE EXTENSION neon_test_utils")
    for t in ("t1", "t2", "t3"):
        primary.safe_psql(f"CREATE TABLE {t}(i int primary key, j int)")
        primary.safe_psql(f"INSERT INTO {t} VALUES (1, 0)")

    conns = [primary.connect(autocommit=False) for _ in range(4)]
    a, b, c, d = (conn.cursor() for conn in conns)

    # Multixid 1: an update and a key-share lock, so that reading the row needs
    # the multixid's members to find the updater.
    a.execute("UPDATE t1 SET j = 1")
    b.execute("SELECT * FROM t1 FOR KEY SHARE")
    mxid1 = int(primary.safe_psql_scalar("SELECT xmax::text::bigint FROM t1"))
    conns[0].commit()
    conns[1].commit()

    # Multixid 2: pause the second locker right before it WAL-logs the multixid
    # (XLOG_MULTIXACT_CREATE_ID is info 0x20 of RM_MULTIXACT_ID, 6).
    a.execute("SELECT * FROM t2 FOR KEY SHARE")
    pid = int(query_scalar(b, "SELECT pg_backend_pid()"))
    gdb_log = test_output_dir / "gdb.log"
    lock_errors: list[BaseException] = []
    locker = None

    def wait_for_gdb(what: str):
        deadline = time.monotonic() + 30
        while what not in gdb_log.read_text():
            text = gdb_log.read_text()
            if "ptrace: Operation not permitted" in text:
                pytest.skip("not permitted to attach to postgres with gdb")
            assert debugger.poll() is None, text
            assert not lock_errors, lock_errors
            assert time.monotonic() < deadline, text
            time.sleep(0.05)

    with gdb_log.open("w") as log_file:
        debugger = subprocess.Popen(
            [
                "gdb",
                "-q",
                "-nx",
                "-p",
                str(pid),
                "-ex",
                "set pagination off",
                "-ex",
                "break XLogInsert if rmid == 6 && info == 32",
                "-ex",
                "continue",
            ],
            stdin=subprocess.PIPE,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_gdb("Continuing.")

            def lock_row():
                try:
                    b.execute("SELECT * FROM t2 FOR KEY SHARE")
                except BaseException as e:
                    lock_errors.append(e)

            locker = threading.Thread(target=lock_row)
            locker.start()
            wait_for_gdb("Breakpoint 1, XLogInsert")

            # Multixid 3 is assigned after 2, but WAL-logged before it
            c.execute("SELECT * FROM t3 FOR KEY SHARE")
            d.execute("SELECT * FROM t3 FOR KEY SHARE")
            mxid3 = int(primary.safe_psql_scalar("SELECT xmax::text::bigint FROM t3"))
            conns[2].commit()
            conns[3].commit()
            assert mxid3 == mxid1 + 2, (mxid1, mxid3)

            primary.safe_psql("SELECT neon_xlogflush()")
            wait_for_wal_insert_lsn(env, primary, env.initial_tenant, env.initial_timeline)

            # Start a replica now, while multixid 2 is still not logged, and read
            # the row locked by multixid 1. Its members end where multixid 2's
            # start.
            replica = env.endpoints.new_replica_start(origin=primary, endpoint_id="replica")
            assert replica.safe_psql("SELECT * FROM t1", options="-c statement_timeout=5000") == [
                (1, 1)
            ]
        finally:
            if debugger.poll() is None:
                assert debugger.stdin is not None
                debugger.stdin.write("delete breakpoints\ndetach\nquit\n")
                debugger.stdin.flush()
                debugger.communicate(timeout=30)
            if locker is not None:
                locker.join(timeout=30)
            for conn in conns:
                conn.rollback()
                conn.close()
