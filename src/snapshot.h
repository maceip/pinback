#ifndef PIN_SNAPSHOT_H
#define PIN_SNAPSHOT_H

/* Per-turn workspace change tracking via an isolated shadow git repo.
 *
 * The shadow repo's GIT_DIR lives under pinback's per-workspace state
 * dir, separate from the workspace tree, so the user's own VCS (if any)
 * is never touched. begin() records the start-of-turn state; diff()
 * reports what changed since; revert() reverse-applies a hunk patch.
 * Entirely agent-independent -- it observes the filesystem, not the
 * agent's tool output. */

#include "util.h"

#include <stdbool.h>
#include <stddef.h>

/* Record the current worktree as the turn baseline (a commit on the
 * shadow repo's HEAD). Initializes the repo at git_dir on first use and
 * installs an exclude list so heavy dirs (node_modules, .git, ...) are
 * not snapshotted. Returns false on hard error. */
bool pin_snapshot_begin(const char *git_dir, const char *work_tree);

/* Append a unified diff (no color) of changes since the last begin() to
 * `out`. out is left empty when nothing changed. Returns false on error. */
bool pin_snapshot_diff(const char *git_dir, const char *work_tree, pin_buf *out);

/* Reverse-apply `patch` (a unified diff: one file, one or more hunks)
 * against the worktree, undoing those changes. errbuf is set on failure. */
bool pin_snapshot_revert(const char *git_dir, const char *work_tree, const char *patch,
                         size_t patch_len, char *errbuf, size_t errcap);

#endif
