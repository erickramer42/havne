#!/usr/bin/env bash
# File/layer: scripts/ci_sccache.sh | Linux and macOS runners, bash 3.2 or later
# Testing: CI only; each job's "Seed the compiler cache" step prints the
#          tier it used and the build step writes it to the run's summary
#
# The compiler cache's seed and pruning for ci.yml's Linux and macOS jobs
# (docs/ci.md, "Caches"); win-build and win-load do the same in PowerShell.
#
#   ci_sccache.sh seed <artifact>
#     With no cache entry of the branch's own (OWN_KEY empty), downloads
#     <artifact> into SCCACHE_DIR from the newest ci.yml run of this
#     repository on dev that uploaded one, or from SEED_RUN's run (a run
#     id: only a writer can dispatch it). Writes SCCACHE_TIER to
#     GITHUB_ENV. Never fails: a seed that can't be had leaves the build
#     cold.
#   ci_sccache.sh prune <prefix> <new key>
#     Deletes this ref's cache entries under <prefix> created before
#     <new key>'s, never another ref's. Never fails.
#
# Environment: GH_TOKEN (actions: read for seed, write for prune),
# GITHUB_REPOSITORY, GITHUB_REF, GITHUB_ENV, SCCACHE_DIR; seed also reads
# REPO_ID (github.repository_id), OWN_KEY and SEED_RUN.

set -u

# The run ids to try, newest first, one per line
seed_candidates() {
  local artifact=$1
  if [[ "${SEED_RUN:-}" =~ ^[0-9]+$ ]]; then
    echo "$SEED_RUN"
    return
  fi
  gh api "repos/$GITHUB_REPOSITORY/actions/artifacts?name=$artifact&per_page=30" --jq "
    [.artifacts[] | select((.expired | not) and .workflow_run.head_branch == \"dev\"
       and (.workflow_run.head_repository_id | tostring) == \"$REPO_ID\")]
    | sort_by(.created_at) | reverse | .[:5][] | .workflow_run.id"
}

# Whether run $1 is one whose artifact may be trusted
trusted_run() {
  local id=$1 info
  info=$(gh api "repos/$GITHUB_REPOSITORY/actions/runs/$id" \
    --jq '.path + " " + .head_repository.full_name + " " + .head_branch') || return 1
  set -- $info
  [ "${1:-}" = .github/workflows/ci.yml ] && [ "${2:-}" = "$GITHUB_REPOSITORY" ] || return 1
  [[ "${SEED_RUN:-}" =~ ^[0-9]+$ ]] || [ "${3:-}" = dev ]
}

seed() {
  local artifact=$1 tier='none (cold build)' id n
  if [ -n "${OWN_KEY:-}" ]; then
    tier="own branch ($OWN_KEY)"
  else
    for id in $(seed_candidates "$artifact" || true); do
      if ! trusted_run "$id"; then
        echo "skipping run $id: not a ci.yml run of $GITHUB_REPOSITORY on dev"
        continue
      fi
      if gh run download "$id" -R "$GITHUB_REPOSITORY" -n "$artifact" -D "$SCCACHE_DIR"; then
        n=$(find "$SCCACHE_DIR" -type f | wc -l | tr -d ' ')
        if [ "$n" -gt 0 ]; then
          tier="seed of run $id ($n files)"
          break
        fi
      fi
      rm -rf "$SCCACHE_DIR"
    done
  fi
  echo "compiler cache tier: $tier"
  echo "SCCACHE_TIER=$tier" >> "$GITHUB_ENV"
}

prune() {
  local prefix=$1 new_key=$2 list new_at
  list=$(gh cache list --ref "$GITHUB_REF" --key "$prefix" --limit 100 \
    --json id,key,createdAt --jq '.[] | "\(.id) \(.createdAt) \(.key)"') || return 0
  new_at=$(printf '%s\n' "$list" | awk -v k="$new_key" '$3 == k { print $2; exit }')
  if [ -z "$new_at" ]; then
    echo 'the new entry is not listed yet; the next run prunes'
    return 0
  fi
  printf '%s\n' "$list" | awk -v k="$new_key" -v t="$new_at" '$3 != k && $2 < t { print $1, $3 }' |
    while read -r id key; do
      echo "deleting $key"
      gh cache delete "$id" || true
    done
}

case "${1:-} $#" in
  'seed 2') seed "$2" ;;
  'prune 3') prune "$2" "$3" ;;
  *) echo "usage: ci_sccache.sh seed <artifact> | prune <prefix> <new key>" >&2; exit 2 ;;
esac
exit 0
