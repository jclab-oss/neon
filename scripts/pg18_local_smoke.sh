#!/usr/bin/env bash
#
# Drive a real Neon deployment with neon_local and exercise it with actual SQL.
#
# The pytest suite covers a lot, but it runs everything through fixtures that set
# their own configuration; this brings the stack up the way an operator would -
# `neon_local init`, `tenant create`, `endpoint start` - and then just uses psql.
# That is how several PG18 problems were originally found: the failure showed up
# in a compute log or a walredo process, not in a test assertion.
#
# Usage:
#   scripts/pg18_local_smoke.sh [PG_VERSION] [REPO_DIR]
#
#   PG_VERSION   v14..v18, default v18
#   REPO_DIR     where to put the deployment, default /tmp/neon_smoke_$PG_VERSION
#
# Expects `make` to have built the binaries and pg_install already.
#
set -uo pipefail

PG_VERSION="${1:-v18}"
REPO_DIR="${2:-/tmp/neon_smoke_${PG_VERSION}}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/target/debug"
PG_INSTALL="${ROOT}/pg_install"
PGBIN="${PG_INSTALL}/${PG_VERSION}/bin"

export LD_LIBRARY_PATH="${PG_INSTALL}/${PG_VERSION}/lib"
export NEON_REPO_DIR="${REPO_DIR}"

TENANTS=2          # separate tenants, each with its own timelines
BRANCHES=2         # branches per tenant, each with its own compute

fail=0
step() { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; fail=$((fail + 1)); }

check() {  # check <description> <expected> <actual>
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}

nl() { "${BIN}/neon_local" "$@"; }

# psql against an endpoint, quiet and script-friendly.
q() {  # q <port> <sql>
  "${PGBIN}/psql" -h 127.0.0.1 -p "$1" -U cloud_admin -d postgres \
    -qtAX -v ON_ERROR_STOP=1 -c "$2" 2>&1
}

cleanup() {
  step "Shutting down"
  nl stop >/dev/null 2>&1 || true
  # neon_local stop leaves computes behind if it errors out. Exclude our own pid:
  # REPO_DIR is on this script's command line, so a bare pkill -f kills the script.
  pkill -f --  "postgres.*${REPO_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

for tool in "${BIN}/neon_local" "${PGBIN}/psql" "${PGBIN}/postgres"; do
  [ -x "$tool" ] || { echo "missing $tool - run 'make all' first"; exit 1; }
done

# One 10-port block per endpoint. neon_local otherwise auto-assigns the http
# ports from a counter that collides with the pg ports we hand out. The base is
# derived from the major version so a v17 control run cannot land on a v18 run's
# ports - the two overlapped when run back to back, because the previous
# deployment is still shutting down when this one starts.
#
# Deliberately below net.ipv4.ip_local_port_range (32768 by default): in the
# 55000s the storage services' own outgoing connections take these ports as
# ephemeral source ports, and the compute then fails to bind with "Address
# already in use" even though nothing is listening on them.
port_base=$((20000 + (${PG_VERSION#v} - 14) * 100))
last_port=$((port_base + TENANTS * BRANCHES * 10))

step "Deployment: ${PG_VERSION} in ${REPO_DIR}, ports ${port_base}-${last_port}"
for _ in $(seq 30); do
  busy=$(ss -ltn 2>/dev/null | awk -v lo="$port_base" -v hi="$last_port" \
    '{split($4,a,":"); p=a[length(a)]+0; if (p>=lo && p<=hi) print p}' | head -3 | tr '\n' ' ')
  [ -z "$busy" ] && break
  echo "  waiting for ports ${busy}to free up"
  sleep 2
done
"${PGBIN}/postgres" --version
rm -rf "${REPO_DIR}"
nl init --force empty-dir-ok >/dev/null || { echo "init failed"; exit 1; }
nl start >/dev/null || { echo "start failed"; exit 1; }
ok "storage stack up"

ep_index=0
declare -a ENDPOINTS=()

for t in $(seq 1 $TENANTS); do
  step "Tenant ${t}"
  # Build the ids as text: these are 128-bit and bash arithmetic is 64-bit, so
  # computing them numerically silently truncates every id to the same value.
  tenant_id=$(printf 'aa%029xa' "$t")
  timeline_id=$(printf 'bb%029xb' "$t")

  nl tenant create --tenant-id "$tenant_id" --timeline-id "$timeline_id" \
     --pg-version "${PG_VERSION#v}" >/dev/null || { bad "tenant $t create"; continue; }
  ok "tenant ${tenant_id:0:8}… created"

  for b in $(seq 1 $BRANCHES); do
    ep="ep-t${t}b${b}"
    port=$((port_base + ep_index * 10))
    ext_http=$((port + 1))
    int_http=$((port + 2))
    ep_index=$((ep_index + 1))

    if [ "$b" -eq 1 ]; then
      branch=main
    else
      branch="br${b}"
      nl timeline branch --tenant-id "$tenant_id" --branch-name "$branch" \
         --ancestor-branch-name main >/dev/null || { bad "branch $branch"; continue; }
    fi

    if ! nl endpoint create "$ep" --tenant-id "$tenant_id" --branch-name "$branch" \
         --pg-version "${PG_VERSION#v}" --pg-port "$port" \
         --external-http-port "$ext_http" --internal-http-port "$int_http" \
         > "${REPO_DIR}/${ep}.create.log" 2>&1; then
      bad "endpoint $ep failed to create"
      sed 's/^/        /' "${REPO_DIR}/${ep}.create.log" | tail -6
      continue
    fi
    if ! nl endpoint start "$ep" > "${REPO_DIR}/${ep}.start.log" 2>&1; then
      bad "endpoint $ep failed to start"
      sed 's/^/        /' "${REPO_DIR}/${ep}.start.log" | tail -6
      continue
    fi
    ENDPOINTS+=("${ep}:${port}:${tenant_id}")
    ok "compute ${ep} on ${branch}, port ${port}"

    # --- the actual exercise -------------------------------------------------

    tbl="t_t${t}b${b}"

    # A branch starts as a copy of its ancestor, so the parent's table must be
    # readable here and carry exactly the rows the parent left behind.
    if [ "$b" -ne 1 ]; then
      check "  inherited ancestor's rows" "17143" \
            "$(q "$port" "SELECT count(*) FROM t_t${t}b1")"
    fi

    v=$(q "$port" "SHOW server_version_num")
    case "$v" in
      "${PG_VERSION#v}"*) ok "  server_version_num=$v" ;;
      *) bad "  server_version_num=$v for ${PG_VERSION}" ;;
    esac

    # DDL + bulk insert: exercises relation extension and WAL generation
    q "$port" "CREATE TABLE $tbl (id bigserial primary key, payload text, n int)" >/dev/null
    q "$port" "INSERT INTO $tbl (payload, n) SELECT repeat('x', 200), g FROM generate_series(1, 20000) g" >/dev/null
    check "  20000 rows inserted" "20000" "$(q "$port" "SELECT count(*) FROM $tbl")"

    # Index build, then a read that must go through it
    q "$port" "CREATE INDEX ${tbl}_n_idx ON $tbl (n)" >/dev/null
    check "  index lookup" "1" "$(q "$port" "SELECT count(*) FROM $tbl WHERE n = 12345")"

    # Update + delete + VACUUM: dirties pages, exercises visibility map and
    # the neon rmgr redo path that PG18 broke.
    q "$port" "UPDATE $tbl SET payload = repeat('y', 200) WHERE n % 10 = 0" >/dev/null
    q "$port" "DELETE FROM $tbl WHERE n % 7 = 0" >/dev/null
    q "$port" "VACUUM (FREEZE, ANALYZE) $tbl" >/dev/null
    check "  rows after delete" "17143" "$(q "$port" "SELECT count(*) FROM $tbl")"

    # Force everything out to the pageserver and read it back cold, so the
    # answer cannot come from local buffers.
    q "$port" "CHECKPOINT" >/dev/null
    sum_before=$(q "$port" "SELECT coalesce(sum(n),0) FROM $tbl")
    nl endpoint stop "$ep" >/dev/null 2>&1
    nl endpoint start "$ep" >/dev/null 2>&1
    check "  checksum survives restart" "$sum_before" "$(q "$port" "SELECT coalesce(sum(n),0) FROM $tbl")"

    # Catalog-heavy work: this is what surfaced the data-checksum problem,
    # because catalogs are the first pages a fresh compute reads.
    # Named per endpoint: a branch inherits its ancestor's schema, so a fixed
    # name would make CREATE SCHEMA fail and the check then pass on the
    # ancestor's objects instead of anything this compute wrote.
    sch="s_t${t}b${b}"
    q "$port" "CREATE SCHEMA $sch; CREATE TABLE $sch.a (x int);
               CREATE VIEW $sch.v AS SELECT * FROM $sch.a;
               CREATE FUNCTION $sch.f() RETURNS int LANGUAGE sql AS 'SELECT 1'" >/dev/null
    check "  catalog relations" "2" \
          "$(q "$port" "SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='$sch'")"
    check "  catalog function" "1" \
          "$(q "$port" "SELECT count(*) FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace WHERE n.nspname='$sch'")"

    # Multixact + subtransactions, which have their own SLRU handling
    q "$port" "BEGIN; SAVEPOINT a; INSERT INTO $sch.a VALUES (1); ROLLBACK TO a;
               INSERT INTO $sch.a VALUES (2); COMMIT" >/dev/null
    check "  subtransaction rollback" "2" "$(q "$port" "SELECT coalesce(max(x),0) FROM $sch.a")"

    # Large value: forces TOAST, i.e. a second relation and more page traffic
    q "$port" "CREATE TABLE ${tbl}_big (id int, blob text);
               INSERT INTO ${tbl}_big SELECT 1, repeat('z', 2000000)" >/dev/null
    check "  toasted value" "2000000" "$(q "$port" "SELECT length(blob) FROM ${tbl}_big")"

    # Dropping a relation inside a transaction puts dropped-statistics items in
    # the commit record. Getting their size wrong made the safekeeper panic
    # decoding the WAL, which took the whole timeline down - and this script
    # missed it entirely, because nothing here used to drop anything.
    q "$port" "CREATE TABLE ${tbl}_drop (id int, t text);
               INSERT INTO ${tbl}_drop SELECT g, repeat('d', 100) FROM generate_series(1, 500) g" >/dev/null
    q "$port" "ANALYZE ${tbl}_drop" >/dev/null
    q "$port" "BEGIN; DROP TABLE ${tbl}_drop; DROP INDEX ${tbl}_n_idx; COMMIT" >/dev/null
    check "  dropped in a transaction" "0" \
          "$(q "$port" "SELECT count(*) FROM pg_class WHERE relname IN ('${tbl}_drop','${tbl}_n_idx')")"

    # Several relations dropped at once, so the commit record carries more than
    # one stats item and a wrong per-item size compounds.
    q "$port" "CREATE TABLE d1(x int); CREATE TABLE d2(x int); CREATE TABLE d3(x int);
               INSERT INTO d1 VALUES (1); INSERT INTO d2 VALUES (1); INSERT INTO d3 VALUES (1);
               ANALYZE d1; ANALYZE d2; ANALYZE d3" >/dev/null
    q "$port" "BEGIN; DROP TABLE d1, d2, d3; COMMIT" >/dev/null
    check "  multi-relation drop" "0" \
          "$(q "$port" "SELECT count(*) FROM pg_class WHERE relname IN ('d1','d2','d3')")"

    # The drops have to survive a round trip through the pageserver, which is
    # where a mis-decoded commit record actually surfaces.
    q "$port" "CHECKPOINT" >/dev/null
    nl endpoint stop "$ep" >/dev/null 2>&1
    nl endpoint start "$ep" >/dev/null 2>&1
    check "  drops survive restart" "$sum_before" "$(q "$port" "SELECT coalesce(sum(n),0) FROM $tbl")"
  done
done

step "Cross-checking branches are independent"
if [ "${#ENDPOINTS[@]}" -ge 2 ]; then
  IFS=: read -r _ p1 tid1 <<< "${ENDPOINTS[0]}"
  IFS=: read -r _ p2 _    <<< "${ENDPOINTS[1]}"
  # Written to the parent AFTER the child branched, so it must not be visible
  # on the child: this is what proves the branches diverged rather than sharing.
  q "$p1" "INSERT INTO t_t1b1 (payload, n) VALUES ('branch-local', 999999)" >/dev/null
  check "write visible on its own branch"   "1" "$(q "$p1" 'SELECT count(*) FROM t_t1b1 WHERE n = 999999')"
  check "write absent on the other branch"  "0" "$(q "$p2" 'SELECT count(*) FROM t_t1b1 WHERE n = 999999')"
fi

step "Storage state"
if [ "${#ENDPOINTS[@]}" -gt 0 ]; then
  nl timeline list --tenant-id "$(echo "${ENDPOINTS[0]}" | cut -d: -f3)" 2>/dev/null | sed 's/^/  /'
else
  echo "  (no endpoints came up)"
fi

step "Scanning service logs for trouble"
# The failures this script exists to catch mostly show up here rather than in a
# query result: walredo dying, pages failing verification, seccomp kills.
for pat in "invalid page" "PM child array" "out of on_proc_exit" \
           "walredo failure" "seccomp" "PANIC" "page verification failed"; do
  n=$(grep -rl "$pat" "${REPO_DIR}" 2>/dev/null | wc -l)
  if [ "$n" -eq 0 ]; then ok "no '$pat'"; else bad "'$pat' in $n log file(s)"; fi
done

biggest=$(find "${REPO_DIR}" -name '*.log' -printf '%s %p\n' 2>/dev/null | sort -rn | head -1)
echo "  largest log: ${biggest:-none}"

step "Result"
if [ "$fail" -eq 0 ]; then
  echo "PASS (${PG_VERSION})"
else
  echo "FAIL (${PG_VERSION}): ${fail} check(s)"
fi
exit "$fail"
