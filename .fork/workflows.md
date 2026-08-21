# GitHub Actions workflows on this fork

This fork of [neondatabase/neon](https://github.com/neondatabase/neon) cannot rely on Neon's
internal CI infrastructure: self-hosted runners (`large`, `small`, `us-east-2`,
`unit-perf-aws-arm`, ...), AWS OIDC roles, a Hetzner-backed build cache, private repos
(`neondatabase/cloud`, `neondatabase/dev-actions`, `neondatabase/proxy-bench`), and a long list
of Neon-internal secrets (staging/production Cloud API keys, internal Postgres result
databases, Slack bot tokens, Docker Hub / ECR / ACR credentials, etc).

This document explains what was done to every workflow under `.github/workflows/` and
`.github/workflows-disabled/`, and what a fork owner needs to configure to make the surviving
workflows run.

## Conventions applied across all modified/new workflows

- **Runners**: every `runs-on` that used to point at a self-hosted or org-specific label was
  replaced with a GitHub-hosted runner. Where the original job had a real reason to want a
  non-default runner (arch-specific builds, macOS, Docker image builds), the runner is now
  parameterized as `${{ vars.RUNNER_<PURPOSE> || '<github-hosted-default>' }}`, so a fork owner
  can point it at a bigger/self-hosted runner via a repository/organization **variable**
  without editing the workflow, while it works out of the box with the default.
  Variables introduced: `RUNNER_DEFAULT`, `RUNNER_BUILD_TOOLS`, `RUNNER_BUILD_TOOLS_ARM64`,
  `RUNNER_RUST_CHECK`, `RUNNER_RUST_CHECK_ARM64`, `RUNNER_MACOS`, `RUNNER_BUILD_AND_TEST`,
  `RUNNER_DOCKER_BUILD`.
- **Container registry**: every image that was pushed/pulled under `ghcr.io/neondatabase/...`,
  `docker.io/neondatabase/...`, or a hardcoded AWS ECR / Azure ACR repository was rewritten to
  `ghcr.io/${{ github.repository_owner }}/...`. AWS ECR and Azure ACR push targets were removed
  entirely (see `_push-to-container-registry.yml` and `pin-build-tools-image.yml`).
- **AWS/Azure**: all `aws-actions/*`, `azure/login`, S3, and ECR/ACR steps were removed from
  every workflow that remains enabled.
- **Slack**: notification steps that depend on `secrets.SLACK_BOT_TOKEN` were either removed or
  made conditional so a missing token doesn't break the job.
- **Caching**: the internal `tespkg/actions-cache` (Hetzner-backed S3) steps were replaced with
  plain `actions/cache`, or with `type=gha` Docker Buildx cache for image builds.

## Active workflows (`.github/workflows/`)

### Kept as-is (already GitHub-hosted, no external dependency)

| Workflow | Role |
|---|---|
| `_meta.yml` | Reusable helper that computes build tags / run-kind for the (now-disabled) main CI pipeline. Kept for reference; currently unused since its only caller (`build_and_test.yml`) was disabled. |
| `actionlint.yml` | Lints all workflow YAML with `actionlint`, and rejects `-latest` runner labels. |
| `check-permissions.yml` | Reusable gate that blocks CI on PRs from unknown forks unless labeled `approved-for-ci-run`. |
| `cleanup-caches-by-a-branch.yml` | Deletes GitHub Actions caches for a branch when its PR is closed. |
| `lint-release-pr.yml` | Lints PRs targeting `release*` branches with a local shell script. |
| `regenerate-pg-setting.yml` | Posts a reminder comment on PRs that touch Postgres GUC files. |

### GitHub App bot token

`fast-forward.yml`, `label-for-external-users.yml`, and `approved-for-ci-run.yml` need to push
commits / open PRs / call the org-membership API in a way that itself triggers further Actions
runs — something the default `GITHUB_TOKEN` cannot do (to prevent infinite loops, pushes and PRs
made with `GITHUB_TOKEN` never trigger other workflow runs). These workflows mint a short-lived
token from a GitHub App at the start of each job:

```yaml
- name: Generate a token
  id: app-token
  uses: actions/create-github-app-token@bcd2ba49218906704ab6c1aa796996da409d3eb1 # v3.2.0
  with:
    app-id: ${{ secrets.GH_AUTOBOT_CLIENT_ID }}
    private-key: ${{ secrets.GH_AUTOBOT_PRIVATE_KEY }}
```

and then use `${{ steps.app-token.outputs.token }}` wherever a token is needed (`GH_TOKEN` env,
`actions/checkout`'s `token:` input, etc). To enable this on a fork, create a GitHub App
(organization or personal account settings → Developer settings → GitHub Apps), install it on
this repository, and add two repository secrets:

- `GH_AUTOBOT_CLIENT_ID` — the App's Client ID (or numeric App ID)
- `GH_AUTOBOT_PRIVATE_KEY` — a private key generated for the App (PEM contents)

The App needs, at minimum, `contents: write` and `pull-requests: write` repository permissions,
and (for `label-for-external-users.yml`) `members: read` organization permission to check
membership.

### Modified (runner/registry/dependency fixes applied)

| Workflow | What changed |
|---|---|
| `fast-forward.yml` | Fast-forward-merges a labeled PR into a `release*` branch. Uses a token minted via `actions/create-github-app-token` (see "GitHub App bot token" below) for the label removal, the merge, and the failure comment. |
| `label-for-external-users.yml` | Labels issues/PRs from non-members as `external`. The org-membership check uses a GitHub App token. |
| `approved-for-ci-run.yml` | Mirrors an external contributor's PR into a local branch/PR so CI can run on it. The checkout, `gh pr`, and `git push` steps all use a GitHub App token. |
| `_check-codestyle-python.yml` | `runs-on` → `vars.RUNNER_DEFAULT \|\| ubuntu-22.04`; Hetzner cache → `actions/cache`. |
| `_check-codestyle-rust.yml` | `runs-on` → `vars.RUNNER_RUST_CHECK[_ARM64]` with GitHub-hosted defaults; Hetzner cache → `actions/cache`; dropped the `prepare-for-subzero` step (private `neondatabase/subzero` dependency — `proxy/Cargo.toml` already ships a local stub for `subzero-core`, so the crate builds fine without it). |
| `_push-to-container-registry.yml` | Removed the `image-map` support for AWS ECR, Azure ACR, and Docker Hub; only GHCR pushes (via `GITHUB_TOKEN`) remain. Dropped the Slack failure notification. |
| `build-build-tools-image.yml` | `build-image` job moved off self-hosted `large`/`large-arm64` to `vars.RUNNER_BUILD_TOOLS[_ARM64] \|\| ubuntu-24.04[-arm]`; dropped Docker Hub login and the `cache.neon.build` registry cache (replaced with `type=gha` Buildx cache); image tag now `ghcr.io/${{ github.repository_owner }}/build-tools`. |
| `build-macos.yml` | Dropped the `prepare-for-subzero` step (see above). Runner parameterized as `vars.RUNNER_MACOS \|\| macos-15` (already GitHub-hosted). Its former caller (`neon_extra_builds.yml`) was disabled, so it is now called from `build-and-test.yml` instead. |
| `cargo-deny.yml` | `runs-on` → `vars.RUNNER_DEFAULT \|\| ubuntu-22.04`; default image → `ghcr.io/${{ github.repository_owner }}/build-tools:pinned`; Slack step now conditional on `vars.SLACK_ON_CALL_DEVPROD_STREAM` being set. |
| `force-test-extensions-upgrade.yml` | `runs-on: small` (self-hosted) → `vars.RUNNER_DEFAULT \|\| ubuntu-22.04`; dropped unused AWS OIDC permission; `docker-compose/test_extensions_upgrade.sh` now pulls images from `ghcr.io/${{ github.repository_owner }}` via `REPOSITORY` env var instead of the upstream default; Slack step now conditional. |
| `pin-build-tools-image.yml` | `image-map` reduced to GHCR-only (`ghcr.io/${{ github.repository_owner }}/build-tools`); all AWS ECR / Azure ACR targets removed; runner parameterized. |
| `pre-merge-checks.yml` | `gh pr edit --add-label fast-forward` now uses the default `GITHUB_TOKEN` (with `pull-requests: write` permission added) — no cross-run-triggering is needed here, so no App token is required. No runner changes needed (already `ubuntu-22.04`). |
| `release.yml` | Replaced the private `neondatabase/dev-actions/release-pr` action (not accessible to forks) with an inline `git`/`gh` implementation: opens a PR from `main` into the target release branch, or, when `cherry-pick` commits are supplied, creates a hotfix branch off the release branch and cherry-picks onto it. Uses the default `GITHUB_TOKEN`. |
| `release-compute.yml`, `release-proxy.yml`, `release-storage.yml` | Unchanged — thin `workflow_call` wrappers around `release.yml`, inherit its fix. |

### New workflows (core build/test/publish, written from scratch)

The upstream build/test/publish pipeline (`build_and_test.yml`, `_build-and-test-locally.yml`,
`build_and_test_fully.yml`, `build_and_test_with_sanitizers.yml`,
`build_and_run_selected_test.yml`, `neon_extra_builds.yml`) is one large, deeply interlinked
system built entirely around self-hosted `large`/`large-metal`/`large-arm64` runners, AWS OIDC,
real S3/Azure remote-storage integration tests, an internal Allure+Postgres test-report
backend, and ECR/ACR image mirroring. It isn't feasible to "fix" in place, so it was left
disabled and replaced with two new, much smaller workflows:

| Workflow | Role |
|---|---|
| `build-and-test.yml` | Builds Neon + one Postgres version (`workflow_dispatch` lets you pick `v14`–`v17`, default `v17`) using the `build-tools` image, runs `cargo test`/`cargo nextest` (Rust unit tests) and the `test_runner/regress` Python suite, all inside a single GitHub-hosted job. Also calls `build-macos.yml` (previously orphaned) and `check-permissions.yml`. Real-S3/Azure-backed tests self-skip since no cloud credentials are configured. Triggers on push to `main`, on every PR, and manually. |
| `docker-publish.yml` | Builds and pushes the `neon` (storage) image and `compute-node-<pg_version>` image(s) to `ghcr.io/${{ github.repository_owner }}` using the existing `Dockerfile` / `compute/compute-node.Dockerfile` (both already parameterize their base image via `REPOSITORY`/`IMAGE`/`TAG` build args, so no Dockerfile changes were needed). Triggers on push to `main` and on `v*` tags; `workflow_dispatch` lets you choose which Postgres versions to build compute images for (default `["v17"]`). |

**Known limitations of the new workflows** (documented here rather than silently glossed over):
- Only one Postgres version is built/tested per run by default (`v17`), not the full matrix of
  compute images. Adjust the `pg_versions`/`pg_version` inputs to cover more.
- `build-and-test.yml` runs a debug build only, on `x64`, with no sanitizers, no ARM
  coverage, and no compatibility-snapshot testing against a previous release.
- Compute image builds (`compute/compute-node.Dockerfile`) are large; they may need a runner
  with more resources than the free-tier default (`vars.RUNNER_DOCKER_BUILD`) provides,
  especially for the full extension set.
- No performance/benchmark reporting, no Allure test reporting — failures surface as failed CI
  jobs plus an uploaded `test-output` artifact on failure.

## Disabled workflows (`.github/workflows-disabled/`)

Moving a workflow file out of `.github/workflows/` is what actually disables it (GitHub Actions
only scans that exact directory); the files are kept for reference/history.

### Superseded by the new build/test/publish workflows above

| Workflow | Why it was disabled instead of patched |
|---|---|
| `_build-and-test-locally.yml` | Core build/test reusable workflow: hardcoded self-hosted `large`/`large-metal`/`large-arm64` runners, AWS OIDC, real S3 (`neon-github-ci-tests`) + real Azure Blob tests, Hetzner cache, internal test-result DB (`REGRESS_TEST_RESULT_CONNSTR_NEW`). |
| `build_and_test.yml` | The ~1600-line master CI orchestrator. Calls almost every other workflow, has its own self-hosted/AWS/Azure jobs, pushes to Docker Hub/ECR/ACR, triggers deploys in the private `neondatabase/infra` and `neondatabase/cloud` repos. |
| `build_and_test_fully.yml`, `build_and_test_with_sanitizers.yml` | Nightly full/sanitizer builds: self-hosted runners, a hardcoded Neon AWS-account ECR base image (`${{ vars.NEON_DEV_AWS_ACCOUNT_ID }}.dkr.ecr...`), AWS OIDC, internal DB. |
| `build_and_run_selected_test.yml` | Manual single-test runner: self-hosted `small` runner, AWS OIDC, internal DB/Allure reporting. |
| `neon_extra_builds.yml` | Orchestrator for macOS build + Rust build-stats; the build-stats job uses AWS OIDC and uploads to a hardcoded S3 bucket (`neon-github-public-dev`). Its useful part (calling `build-macos.yml`) was moved into `build-and-test.yml`. |

### Depend on Neon's own staging/production Cloud API, internal DBs, or self-hosted hardware

| Workflow | Dependency |
|---|---|
| `cloud-regress.yml` | Self-hosted `us-east-2` runner, AWS OIDC, Neon staging API + project IDs, Slack. |
| `cloud-extensions.yml` | Self-hosted `us-east-2` runner, live Neon Cloud staging project creation, Slack. |
| `pg-clients.yml` | AWS OIDC, Neon staging Cloud API, internal result DB, Slack. |
| `random-ops-test.yml` | Self-hosted `small` runner, AWS OIDC, Neon staging Cloud API, internal result DB. |
| `ingest_benchmark.yml`, `large_oltp_benchmark.yml`, `large_oltp_growth.yml`, `benchbase_tpcc.yml`, `benchmarking.yml`, `_benchmarking_preparation.yml` | Self-hosted `us-east-2`/`eastus2` runners, AWS OIDC, Neon staging/production Cloud API and dozens of internal connection-string secrets (RDS/Aurora/staging), Slack. |
| `periodic_pagebench.yml` | Self-hosted dedicated ARM hardware (`unit-perf-aws-arm`), AWS OIDC + S3 snapshot storage, internal perf DB/Grafana. |
| `proxy-benchmark.yml` | Self-hosted ARM hardware, checks out the private `neondatabase/proxy-bench` repo, internal perf DB/Grafana. |
| `report-workflow-stats-batch.yml` | Writes to an internal Neon-hosted Postgres DB via a private `neondatabase/gh-workflow-stats-action`. |
| `release-notify.yml` | Private `neondatabase/dev-actions/release-pr-notify` action + Neon's internal Slack bot/channel. |
| `trigger-e2e-tests.yml` | Triggers workflows in the private `neondatabase/cloud` repository (`gh workflow --repo .../cloud run ...`) — has no equivalent on a fork. |

None of these are meaningfully "fixable" by swapping runners or registries: they exist to
exercise Neon's actual hosted product (staging/production control plane), Neon-internal
reporting databases, or dedicated benchmark hardware that a fork does not have.

## Required repository configuration for a fork owner

- **Secrets**: `GH_AUTOBOT_CLIENT_ID` and `GH_AUTOBOT_PRIVATE_KEY` (see "GitHub App bot token"
  above) if you want `fast-forward.yml`, `label-for-external-users.yml`, and
  `approved-for-ci-run.yml` to work. `GITHUB_TOKEN` (built in) is sufficient for everything else
  that remains enabled.
- **Variables** (all optional — every one has a GitHub-hosted default):
  `RUNNER_DEFAULT`, `RUNNER_BUILD_TOOLS`, `RUNNER_BUILD_TOOLS_ARM64`, `RUNNER_RUST_CHECK`,
  `RUNNER_RUST_CHECK_ARM64`, `RUNNER_MACOS`, `RUNNER_BUILD_AND_TEST`, `RUNNER_DOCKER_BUILD`,
  `SLACK_ON_CALL_DEVPROD_STREAM`, `SLACK_ON_CALL_QA_STAGING_STREAM` (only needed to re-enable
  the optional Slack notifications in `cargo-deny.yml` / `force-test-extensions-upgrade.yml`).
- **Packages**: ensure the repository is allowed to publish GitHub Container Registry (GHCR)
  packages (Settings → Actions → General → Workflow permissions → "Read and write
  permissions", and package visibility as desired).
