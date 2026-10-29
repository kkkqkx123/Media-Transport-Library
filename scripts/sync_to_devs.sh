#!/usr/bin/env bash
# Sync local git changes (working tree + staged) file-by-file to the compile server.
# System libraries/deps are synced manually via scp (out of scope).
# Commits happen on the compile server via git (origin = gitlab), then pull locally.
set -euo pipefail

REMOTE="${REMOTE:-devs}"
REMOTE_DIR="${REMOTE_DIR:-/data/kkkqkx/mtl}"
GITLAB_URL="${GITLAB_URL:-http://kkkqkx123:1234567kk@10.10.20.217/kkkqkx123/mtl-rk-ids.git}"
BRANCH="${BRANCH:-$(git rev-parse --abbrev-ref HEAD)}"

usage() {
  echo "Usage: $0 [--init-git] [--all] [--dry-run]"
  echo "  default: sync files changed vs HEAD + staged"
  echo "  --all: sync entire tracked tree"
  echo "  --init-git: ensure remote dir is a git repo with origin=gitlab"
  echo "  --dry-run: print only"
}

INIT_GIT=0; ALL=0; DRY=0
for a in "$@"; do
  case "$a" in
    --init-git) INIT_GIT=1;;
    --all) ALL=1;;
    --dry-run) DRY=1;;
    -h|--help) usage; exit 0;;
    *) echo "unknown arg $a"; usage; exit 1;;
  esac
done

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

# Collect changed files (null-delimited, dedup)
tmp="$(mktemp)"; trap 'rm -f "$tmp"' EXIT
if [ "$ALL" = "1" ]; then
  git ls-files -z > "$tmp"
else
  {
    git diff --name-only -z HEAD
    git diff --cached --name-only -z
    git ls-files --others --exclude-standard -z
  } | sort -zu > "$tmp"
fi

# Deletions
del_tmp="$(mktemp)"; trap 'rm -f "$tmp" "$del_tmp"' EXIT
if [ "$ALL" != "1" ]; then
  {
    git diff --name-only -z --diff-filter=D HEAD
    git diff --cached --name-only -z --diff-filter=D
  } | sort -zu > "$del_tmp"
fi

echo "Changed files: $(tr '\0' '\n' < "$tmp" | grep -c . || true)"

if [ "$INIT_GIT" = "1" ]; then
  echo "Initializing git on $REMOTE:$REMOTE_DIR ..."
  ssh "$REMOTE" "mkdir -p '$REMOTE_DIR' && cd '$REMOTE_DIR' && \
    if [ ! -d .git ]; then git init; fi && \
    if ! git remote get-url origin >/dev/null 2>&1; then git remote add origin '$GITLAB_URL'; else git remote set-url origin '$GITLAB_URL'; fi && \
    git fetch origin '$BRANCH' 2>&1 | tail -5; \
    git symbolic-ref HEAD refs/heads/$BRANCH 2>/dev/null || true"
fi

# Ensure remote base exists
ssh "$REMOTE" "mkdir -p '$REMOTE_DIR'"

# Sync files
while IFS= read -r -d '' f; do
  [ -z "$f" ] && continue
  if [ ! -f "$f" ]; then continue; fi
  rdir="$(dirname "$REMOTE_DIR/$f")"
  if [ "$DRY" = "1" ]; then
    echo "PUT $f"
  else
    ssh "$REMOTE" "mkdir -p '$rdir'" >/dev/null
    scp -q "$f" "$REMOTE:$REMOTE_DIR/$f"
    echo "PUT $f"
  fi
done < "$tmp"

# Handle deletions
while IFS= read -r -d '' f; do
  [ -z "$f" ] && continue
  if [ "$DRY" = "1" ]; then
    echo "DEL $f"
  else
    ssh "$REMOTE" "rm -f '$REMOTE_DIR/$f'" && echo "DEL $f"
  fi
done < "$del_tmp"

echo "Done. On compile server run: cd $REMOTE_DIR && git add -A && git commit && git push origin $BRANCH"
