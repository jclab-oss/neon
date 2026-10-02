from __future__ import annotations

import os
from contextlib import closing
from pathlib import Path

import psycopg2
import pytest
from fixtures.common_types import Lsn, TenantId, TimelineId
from fixtures.neon_fixtures import (
    NeonEnv,
    NeonEnvBuilder,
    PgBin,
    PgProtocol,
)
from fixtures.pageserver.http import PageserverApiException, PageserverHttpClient


def assert_client_authorized(env: NeonEnv, http_client: PageserverHttpClient):
    http_client.timeline_create(
        pg_version=env.pg_version,
        tenant_id=env.initial_tenant,
        new_timeline_id=TimelineId.generate(),
        ancestor_timeline_id=env.initial_timeline,
    )


def assert_client_not_authorized(env: NeonEnv, http_client: PageserverHttpClient):
    with pytest.raises(
        PageserverApiException,
        match="Forbidden: JWT authentication error",
    ):
        assert_client_authorized(env, http_client)


def test_pageserver_auth(neon_env_builder: NeonEnvBuilder):
    neon_env_builder.auth_enabled = True
    env = neon_env_builder.init_start()

    ps = env.pageserver

    tenant_token = env.auth_keys.generate_tenant_token(env.initial_tenant)
    tenant_http_client = env.pageserver.http_client(tenant_token)
    invalid_tenant_token = env.auth_keys.generate_tenant_token(TenantId.generate())
    invalid_tenant_http_client = env.pageserver.http_client(invalid_tenant_token)

    pageserver_token = env.auth_keys.generate_pageserver_token()
    pageserver_http_client = env.pageserver.http_client(pageserver_token)

    # this does not invoke auth check and only decodes jwt and checks it for validity
    # check both tokens
    ps.safe_psql("set FOO", password=tenant_token)
    ps.safe_psql("set FOO", password=pageserver_token)

    # tenant can create branches
    assert_client_authorized(env, tenant_http_client)

    # console can create branches for tenant
    assert_client_authorized(env, pageserver_http_client)

    # fail to create branch using token with different tenant_id
    with pytest.raises(PageserverApiException, match="Forbidden: JWT authentication error"):
        assert_client_authorized(env, invalid_tenant_http_client)

    # create tenant using management token
    env.pageserver.tenant_create(TenantId.generate(), auth_token=pageserver_token)

    # fail to create tenant using tenant token
    with pytest.raises(
        PageserverApiException,
        match="Forbidden: JWT authentication error",
    ):
        env.pageserver.tenant_create(TenantId.generate(), auth_token=tenant_token)


def test_compute_auth_to_pageserver(neon_env_builder: NeonEnvBuilder):
    neon_env_builder.auth_enabled = True
    neon_env_builder.num_safekeepers = 3
    env = neon_env_builder.init_start()

    branch = "test_compute_auth_to_pageserver"
    env.create_branch(branch)
    endpoint = env.endpoints.create_start(branch)

    with closing(endpoint.connect()) as conn:
        with conn.cursor() as cur:
            # we rely upon autocommit after each statement
            # as waiting for acceptors happens there
            cur.execute("CREATE TABLE t(key int primary key, value text)")
            cur.execute("INSERT INTO t SELECT generate_series(1,100000), 'payload'")
            cur.execute("SELECT sum(key) FROM t")
            assert cur.fetchone() == (5000050000,)


def test_pageserver_multiple_keys(neon_env_builder: NeonEnvBuilder):
    neon_env_builder.auth_enabled = True
    env = neon_env_builder.init_start()
    env.pageserver.allowed_errors.extend(
        [".*Authentication error: InvalidSignature.*", ".*Unauthorized: malformed jwt token.*"]
    )

    pageserver_token_old = env.auth_keys.generate_pageserver_token()
    pageserver_http_client_old = env.pageserver.http_client(pageserver_token_old)

    pageserver_http_client_old.reload_auth_validation_keys()

    # This test is to ensure that the pageserver supports multiple keys.
    # The neon_local tool generates one key pair at a hardcoded path by default.
    # As a preparation for our test, move the public key of the key pair into a
    # directory at the same location as the hardcoded path by:
    # 1. moving the file at `configured_pub_key_path` to a temporary location
    # 2. creating a new directory at `configured_pub_key_path`
    # 3. moving the file from the temporary location into the newly created directory
    configured_pub_key_path = Path(env.repo_dir) / "auth_public_key.pem"
    os.rename(configured_pub_key_path, Path(env.repo_dir) / "auth_public_key.pem.file")
    os.mkdir(configured_pub_key_path)
    os.rename(
        Path(env.repo_dir) / "auth_public_key.pem.file",
        configured_pub_key_path / "auth_public_key_old.pem",
    )

    # Add a new key pair
    # This invalidates env.auth_keys and makes them be regenerated
    env.regenerate_keys_at(
        Path("auth_private_key.pem"), Path("auth_public_key.pem/auth_public_key_new.pem")
    )

    # Reload the keys on the pageserver side
    pageserver_http_client_old.reload_auth_validation_keys()

    # We can continue doing things using the old token
    assert_client_authorized(env, pageserver_http_client_old)

    pageserver_token_new = env.auth_keys.generate_pageserver_token()
    pageserver_http_client_new = env.pageserver.http_client(pageserver_token_new)

    # The new token also works
    assert_client_authorized(env, pageserver_http_client_new)

    # Remove the old token and reload
    os.remove(Path(env.repo_dir) / "auth_public_key.pem" / "auth_public_key_old.pem")
    pageserver_http_client_old.reload_auth_validation_keys()

    # Reloading fails now with the old token, but the new token still works
    assert_client_not_authorized(env, pageserver_http_client_old)
    assert_client_authorized(env, pageserver_http_client_new)


def test_pageserver_key_reload(neon_env_builder: NeonEnvBuilder):
    neon_env_builder.auth_enabled = True
    env = neon_env_builder.init_start()
    env.pageserver.allowed_errors.extend(
        [".*Authentication error: InvalidSignature.*", ".*Unauthorized: malformed jwt token.*"]
    )
    pageserver_token_old = env.auth_keys.generate_pageserver_token()
    pageserver_http_client_old = env.pageserver.http_client(pageserver_token_old)

    pageserver_http_client_old.reload_auth_validation_keys()

    # Regenerate the keys
    env.regenerate_keys_at(Path("auth_private_key.pem"), Path("auth_public_key.pem"))

    # Reload the keys on the pageserver side
    pageserver_http_client_old.reload_auth_validation_keys()

    # Next attempt fails as we use the old auth token
    with pytest.raises(
        PageserverApiException,
        match="Forbidden: JWT authentication error",
    ):
        pageserver_http_client_old.reload_auth_validation_keys()

    # same goes for attempts trying to create a timeline
    assert_client_not_authorized(env, pageserver_http_client_old)

    pageserver_token_new = env.auth_keys.generate_pageserver_token()
    pageserver_http_client_new = env.pageserver.http_client(pageserver_token_new)

    # timeline creation works with the new token
    assert_client_authorized(env, pageserver_http_client_new)

    # reloading also works with the new token
    pageserver_http_client_new.reload_auth_validation_keys()


@pytest.mark.parametrize("auth_enabled", [False, True])
def test_auth_failures(neon_env_builder: NeonEnvBuilder, auth_enabled: bool):
    neon_env_builder.auth_enabled = auth_enabled
    env = neon_env_builder.init_start()

    branch = f"test_auth_failures_auth_enabled_{auth_enabled}"
    timeline_id = env.create_branch(branch)
    env.endpoints.create_start(branch)

    tenant_token = env.auth_keys.generate_tenant_token(env.initial_tenant)
    invalid_tenant_token = env.auth_keys.generate_tenant_token(TenantId.generate())
    pageserver_token = env.auth_keys.generate_pageserver_token()
    safekeeper_token = env.auth_keys.generate_safekeeper_token()

    def check_connection(
        pg_protocol: PgProtocol, command: str, expect_success: bool, **conn_kwargs
    ):
        def op():
            with closing(pg_protocol.connect(**conn_kwargs)) as conn:
                with conn.cursor() as cur:
                    cur.execute(command)

        if expect_success:
            op()
        else:
            with pytest.raises(psycopg2.Error):
                op()

    def check_pageserver(expect_success: bool, **conn_kwargs):
        check_connection(
            env.pageserver,
            f"pagestream_v2 {env.initial_tenant} {env.initial_timeline}",
            expect_success,
            **conn_kwargs,
        )

    check_pageserver(not auth_enabled)
    if auth_enabled:
        check_pageserver(True, password=tenant_token)

        env.pageserver.allowed_errors.append(".*Tenant id mismatch. Permission denied.*")
        check_pageserver(False, password=invalid_tenant_token)

        check_pageserver(True, password=pageserver_token)

        env.pageserver.allowed_errors.append(".*JWT scope '.+' is ineligible for Pageserver auth.*")
        check_pageserver(False, password=safekeeper_token)

    def check_safekeeper(expect_success: bool, **conn_kwargs):
        check_connection(
            PgProtocol(
                host="localhost",
                port=env.safekeepers[0].port.pg,
                options=f"ztenantid={env.initial_tenant} ztimelineid={timeline_id}",
            ),
            "IDENTIFY_SYSTEM",
            expect_success,
            **conn_kwargs,
        )

    check_safekeeper(not auth_enabled)
    if auth_enabled:
        check_safekeeper(True, password=tenant_token)
        check_safekeeper(False, password=invalid_tenant_token)
        check_safekeeper(False, password=pageserver_token)
        check_safekeeper(True, password=safekeeper_token)


def test_tenant_read_only_token(
    neon_env_builder: NeonEnvBuilder, pg_bin: PgBin, test_output_dir: Path
):
    """
    A tenant_read_only token grants the page service requests that read the tenant's data, and
    nothing else: no pageserver management API, no LSN leases, and no safekeeper access.
    """
    neon_env_builder.auth_enabled = True
    env = neon_env_builder.init_start()
    tenant_id = env.initial_tenant
    timeline_id = env.initial_timeline

    # Make sure the timeline exists on the safekeepers.
    endpoint = env.endpoints.create_start("main")
    endpoint.safe_psql("CREATE TABLE t AS SELECT generate_series(1, 100) AS x")
    endpoint.stop()

    read_only_token = env.auth_keys.generate_tenant_read_only_token(tenant_id)
    other_read_only_token = env.auth_keys.generate_tenant_read_only_token(TenantId.generate())
    tenant_token = env.auth_keys.generate_tenant_token(tenant_id)
    pageserver_token = env.auth_keys.generate_pageserver_token()

    env.pageserver.allowed_errors.extend(
        [
            ".*Tenant id mismatch. Permission denied.*",
            ".*JWT scope 'TenantReadOnly' only permits reading tenant data.*",
        ]
    )

    def pageserver_query(query: str, token: str):
        with closing(env.pageserver.connect(password=token)) as conn:
            with conn.cursor() as cur:
                cur.execute(query)

    # Page requests are allowed, but only for the token's tenant.
    pagestream = f"pagestream_v2 {tenant_id} {timeline_id}"
    pageserver_query(pagestream, read_only_token)
    with pytest.raises(psycopg2.Error, match="JWT authentication error"):
        pageserver_query(pagestream, other_read_only_token)

    # So are basebackups.
    basebackup_tar = test_output_dir / "basebackup.tar"
    pg_bin.run_capture(
        [
            "psql",
            "--no-psqlrc",
            env.pageserver.connstr(password=read_only_token),
            "-c",
            f"basebackup {tenant_id} {timeline_id}",
            "-o",
            str(basebackup_tar),
        ]
    )
    assert basebackup_tar.stat().st_size > 0

    # LSN leases hold back GC, so they require a read-write token.
    pageserver_http = env.pageserver.http_client(auth_token=pageserver_token)
    lsn = Lsn(pageserver_http.timeline_detail(tenant_id, timeline_id)["last_record_lsn"])
    lease = f"lease lsn {tenant_id} {timeline_id} {lsn}"
    with pytest.raises(psycopg2.Error, match="JWT authentication error"):
        pageserver_query(lease, read_only_token)
    pageserver_query(lease, tenant_token)

    # The management API is off limits, even for reads.
    read_only_http = env.pageserver.http_client(auth_token=read_only_token)
    assert_client_not_authorized(env, read_only_http)
    with pytest.raises(PageserverApiException, match="Forbidden: JWT authentication error"):
        read_only_http.timeline_detail(tenant_id, timeline_id)
    with pytest.raises(PageserverApiException, match="Forbidden: JWT authentication error"):
        read_only_http.timeline_lsn_lease(tenant_id, timeline_id, lsn)

    # Safekeepers reject read-only tokens on both of their libpq ports.
    sk = env.safekeepers[0]
    for port in [sk.port.pg, sk.port.pg_tenant_only]:
        sk_pg = PgProtocol(
            host="localhost",
            port=port,
            options=f"ztenantid={tenant_id} ztimelineid={timeline_id}",
        )

        def identify_system(token: str, sk_pg: PgProtocol = sk_pg):
            with closing(sk_pg.connect(password=token)) as conn:
                with conn.cursor() as cur:
                    cur.execute("IDENTIFY_SYSTEM")

        identify_system(tenant_token)
        with pytest.raises(psycopg2.Error):
            identify_system(read_only_token)
