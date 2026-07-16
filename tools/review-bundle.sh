#!/usr/bin/env bash
# review-bundle.sh — produce (and self-check) a reviewer handoff bundle for one task.
#
# Usage:
#   ./review-bundle.sh begin <TASK-ID>            # run BEFORE dispatching the task
#   ./review-bundle.sh end   <TASK-ID> [--build]  # run AFTER the implementation pass
#
# Produces .ai/<TASK-ID>_review.tar.gz containing:
#   <TASK-ID>.diff     full base->worktree diff, INCLUDING new/untracked files
#   manifest.json      base/head commits, files touched, declared-vs-found check
#   <TASK-ID>.json     the task spec (if found under tasks/ or repo root)
#   build.log/test.log/lint.log  (if present or produced with --build)
#
# The bundle FAILS (exit 3) if the diff does not contain every file the task
# declared under files.create / files.modify — the guard that stops an empty or
# wrong-range diff from masquerading as a completed task.
#
# Requires: git, python3.  State lives in .ai/ (gitignored).

set -euo pipefail

AI_DIR=".ai"
die() { echo "ERROR: $*" >&2; exit 1; }

command -v git     >/dev/null || die "git not found"
command -v python3 >/dev/null || die "python3 not found"
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "not inside a git repository"

cmd="${1:-}"; id="${2:-}"
[ -n "$cmd" ] && [ -n "$id" ] || die "usage: review-bundle.sh begin|end <TASK-ID> [--build]"
mkdir -p "$AI_DIR"
base_file="$AI_DIR/$id.base"

find_task_json() {
  for p in "tasks/$id.json" "$id.json"; do
    [ -f "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}

# ── begin ─────────────────────────────────────────────────────────────────────
if [ "$cmd" = "begin" ]; then
  git rev-parse HEAD > "$base_file"
  echo "Recorded base $(cat "$base_file") for $id."
  echo "Dispatch the task now. When the pass finishes: $0 end $id"
  exit 0
fi

[ "$cmd" = "end" ] || die "unknown command: $cmd (expected begin|end)"
[ -f "$base_file" ] || die "no base recorded for $id — run '$0 begin $id' before the pass"
base="$(cat "$base_file")"

out_dir="$AI_DIR/${id}_review"
rm -rf "$out_dir"; mkdir -p "$out_dir"
diff_path="$out_dir/$id.diff"

# ── build the diff: base -> current worktree, plus untracked (new) files ───────
# Does NOT touch the git index (uses --no-index for untracked files).
{
  git diff --patch "$base" -- . || true
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    case "$f" in "$AI_DIR"/*) continue;; esac   # never include the tool's own state
    git diff --no-index --patch -- /dev/null "$f" || true
  done < <(git ls-files --others --exclude-standard)
} > "$diff_path" 2>/dev/null || true

head_commit="$(git rev-parse HEAD)"
task_json="$(find_task_json || true)"
[ -n "$task_json" ] && cp "$task_json" "$out_dir/"

# ── validate the diff against the task's declared files ───────────────────────
if python3 - "$id" "$diff_path" "${task_json:-}" "$base" "$head_commit" "$out_dir" <<'PY'
import json, sys, re, os
task_id, diff_path, task_json, base, head, out_dir = sys.argv[1:7]
diff = open(diff_path, encoding="utf-8", errors="replace").read()
# capture the b/ path of every changed file (correct for new files too)
touched = set(m for m in re.findall(r'^diff --git a/.+? b/(.+)$', diff, re.M) if m != "dev/null")
create, modify = [], []
if task_json and os.path.exists(task_json):
    files = json.load(open(task_json)).get("files", {})
    create, modify = files.get("create", []), files.get("modify", [])
miss_c = [f for f in create if f not in touched]
miss_m = [f for f in modify if f not in touched]
json.dump({
    "task_id": task_id, "base": base, "head": head,
    "files_touched": sorted(touched),
    "expected_create": create, "expected_modify": modify,
    "missing_create": miss_c, "missing_modify": miss_m,
    "diff_empty": not touched,
}, open(os.path.join(out_dir, "manifest.json"), "w"), indent=2)
problems = []
if not touched: problems.append("diff is EMPTY — no files changed")
if not task_json: problems.append("task JSON not found (tasks/%s.json) — cannot verify declared files" % task_id)
if miss_c: problems.append("declared NEW files missing from diff: " + ", ".join(miss_c))
if miss_m: problems.append("declared MODIFIED files missing from diff: " + ", ".join(miss_m))
if problems:
    print("BUNDLE CHECK: FAIL")
    for p in problems: print("  - " + p)
    sys.exit(3)
print("BUNDLE CHECK: OK — diff contains all declared files (%d files touched)" % len(touched))
PY
then check_rc=0; else check_rc=$?; fi

# ── optional build + log collection ───────────────────────────────────────────
if [ "${3:-}" = "--build" ]; then
  echo "Running build (requires the devkitPro environment)..."
  if ( mkdir -p build && cd build && cmake .. -DPLATFORM=Switch && make -j"$(nproc)" ) > "$out_dir/build.log" 2>&1; then
    echo "build: OK"
  else
    echo "build: FAILED (see build.log)"
  fi
fi
for L in build.log test.log lint.log; do [ -f "$L" ] && cp "$L" "$out_dir/"; done

# ── bundle ────────────────────────────────────────────────────────────────────
tar -czf "$AI_DIR/${id}_review.tar.gz" -C "$AI_DIR" "${id}_review"
echo "Bundle: $AI_DIR/${id}_review.tar.gz"
if [ "$check_rc" -ne 0 ]; then
  echo "NOTE: bundle check FAILED — fix the diff/implementation before handing off."
  exit "$check_rc"
fi
echo "Upload $AI_DIR/${id}_review.tar.gz for review."
