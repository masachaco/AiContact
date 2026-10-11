#!/bin/sh
# Installs the AI Contact skill for Claude Code (~/.claude/skills/aicontact).
# Use --link to symlink instead of copy (edits in the repo take effect immediately).
set -e
src="$(cd "$(dirname "$0")" && pwd)/aicontact"
dest="${CLAUDE_SKILLS_DIR:-$HOME/.claude/skills}/aicontact"
mkdir -p "$(dirname "$dest")"
rm -rf "$dest"
if [ "$1" = "--link" ]; then ln -s "$src" "$dest"; else cp -R "$src" "$dest"; fi
echo "installed: $dest"
