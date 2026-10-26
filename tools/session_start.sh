#!/bin/sh
# session_start.sh - what every work session on the port starts with
# (docs/DECISIONS.md D-010): where we are, what changed, what runs, what is next.
cd "$(dirname "$0")/.." || exit 1
echo "== $(pwd)"
echo "== last commits"; git log --oneline -15
echo "== uncommitted"; git status --short | head -20
echo "== last session (docs/PROGRESS.md)"; awk '/^## /{n++} n==1' docs/PROGRESS.md
echo "== next milestones"; .venv/bin/python tools/checklist.py --next 5
echo "== background work"
ps -axo pid=,etime=,command= | grep -E "[a]nalyzeHeadless|Python tools/(analyze|entries)|[t]ranslate.py --out" \
    | grep -v "zsh -c" | sed -E 's#[^ ]*/(Python|bash) ##; s#/[^ ]*/(tools|kit|support)/#\1/#g' | cut -c1-110 | sed 's/^/  /'
[ -f analysis-run.log ] && { echo "  analysis-run.log:"; tail -3 analysis-run.log | sed 's/^/    /'; }
echo "== tools"
for t in mono cmake ninja upx; do command -v "$t" >/dev/null || echo "  MISSING: $t"; done
[ -x "$HOME/tools/ghidra_12.1.3_PUBLIC/support/analyzeHeadless" ] || echo "  MISSING: ~/tools/ghidra_12.1.3_PUBLIC"
[ -f "$HOME/tools/ghidra_12.1.3_PUBLIC/Ghidra/Features/Decompiler/build/os/mac_arm_64/decompile" ] || echo "  MISSING: Ghidra arm64 decompiler (gradle buildNatives in support/gradle)"
[ -x /opt/homebrew/opt/openjdk@21/bin/java ] || echo "  MISSING: openjdk@21"
.venv/bin/python -c "import capstone, pefile" 2>/dev/null || echo "  MISSING: .venv with kit/tools/requirements.txt"
echo "== baseline"
.venv/bin/python -c "
import sys; sys.path.insert(0, 'kit/tools'); import game_config
c = game_config.load('.'); print('  game.toml loads: %d aux modules' % len(c['aux_modules']))"
[ -f stage/manifest.json ] && echo "  stage/ present" || echo "  stage/ missing: run tools/stage.py"
