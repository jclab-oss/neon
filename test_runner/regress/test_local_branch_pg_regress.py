"""
Run the PostgreSQL regression tests in a local branch.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

import pytest
from fixtures.neon_fixtures import wait_for_last_flush_lsn
from fixtures.utils import run_only_on_default_postgres

if TYPE_CHECKING:
    from pathlib import Path

    from fixtures.neon_fixtures import NeonEnv, PgBin
    from pytest import CaptureFixture


@pytest.mark.timeout(3000)  # Contains many sub-tests, is slow in debug builds
@run_only_on_default_postgres("slow, and test_local_branch covers all versions")
def test_local_branch_pg_regress(
    neon_simple_env: NeonEnv,
    test_output_dir: Path,
    pg_bin: PgBin,
    capsys: CaptureFixture[str],
    base_dir: Path,
    pg_distrib_dir: Path,
):
    """
    Run the main PostgreSQL regression tests in a local branch, and check that
    the result survives a crash.
    """
    env = neon_simple_env

    # Start the local branch from a branch with some data in it
    main = env.endpoints.create_start("main")
    main.safe_psql("CREATE DATABASE regression")
    main.safe_psql(
        "CREATE TABLE from_main AS SELECT g AS id FROM generate_series(1, 10000) g",
        dbname="regression",
    )
    lsn = wait_for_last_flush_lsn(env, main, env.initial_tenant, env.initial_timeline)
    env.create_branch("anchor", ancestor_branch_name="main", ancestor_start_lsn=lsn)
    endpoint = env.endpoints.create_start(
        "anchor",
        local_branch=True,
        config_lines=["neon.regress_test_mode = true"],
    )

    runpath = test_output_dir / "regress"
    (runpath / "testtablespace").mkdir(parents=True)
    build_path = pg_distrib_dir / f"../build/{env.pg_version.v_prefixed}/src/test/regress"
    src_path = base_dir / f"vendor/postgres-{env.pg_version.v_prefixed}/src/test/regress"
    bindir = pg_distrib_dir / f"v{env.pg_version}/bin"
    # The 'database' test moves a database to another tablespace, which a local
    # branch doesn't support.
    schedule = runpath / "parallel_schedule"
    lines = (src_path / "parallel_schedule").read_text().splitlines()
    schedule.write_text(
        "\n".join(" ".join(word for word in line.split() if word != "database") for line in lines)
        + "\n"
    )

    pg_regress_command = [
        str(build_path / "pg_regress"),
        "--use-existing",
        f"--bindir={bindir}",
        f"--dlpath={build_path}",
        f"--schedule={schedule}",
        f"--inputdir={src_path}",
    ]
    env_vars = {
        "PGPORT": str(endpoint.default_options["port"]),
        "PGUSER": endpoint.default_options["user"],
        "PGHOST": endpoint.default_options["host"],
    }
    with capsys.disabled():
        pg_bin.run(pg_regress_command, env=env_vars, cwd=runpath)

    def dump(name: str) -> str:
        output = test_output_dir / f"{name}.sql"
        pg_bin.run_capture(
            [
                "pg_dump",
                "--no-sync",
                # A crash empties unlogged tables and sequences
                "--no-unlogged-table-data",
                "-d",
                endpoint.connstr(dbname="regression"),
                "-f",
                str(output),
            ]
        )
        # pg_dump adds \restrict lines with a random key
        lines = output.read_text().splitlines()
        return "\n".join(
            line for line in lines if not line.startswith(("\\restrict", "\\unrestrict"))
        )

    before = dump("before-crash")
    endpoint.stop(mode="immediate")
    endpoint.start()
    after = dump("after-crash")
    assert before == after

    assert endpoint.safe_psql("SELECT count(*) FROM from_main", dbname="regression") == [(10000,)]
