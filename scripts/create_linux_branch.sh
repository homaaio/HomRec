#!/usr/bin/env bash
# scripts/create_linux_branch.sh - creates the `linux-raw` branch and commits the Linux-port
# groundwork onto it. Run it from the repository (any folder inside it) AFTER unpacking the
# groundwork files over your working tree. Uncommitted changes travel with you to the new branch.
#
#   scripts/create_linux_branch.sh            # create branch + commit locally
#   scripts/create_linux_branch.sh --push     # ... and push it to origin (sets upstream)
#
# Only the paths listed below are committed - any other edits you have in the tree are left alone.
set -euo pipefail

BRANCH="${BRANCH:-linux-raw}"

cd "$(git rev-parse --show-toplevel)"

if git show-ref --verify --quiet "refs/heads/$BRANCH"; then
    echo "Branch '$BRANCH' already exists. Switching to it."
    git switch "$BRANCH"
else
    echo "Creating branch '$BRANCH' from $(git rev-parse --abbrev-ref HEAD)"
    git switch -c "$BRANCH"
fi

PATHS=(
    docs/LINUX_PORT_PLAN.md
    src/platform
    cmake/LinuxPort.cmake
    CMakeLists.txt
    Makefile
    README.md
    scripts/README.md
    scripts/create_linux_branch.sh
    scripts/create_linux_branch.bat
    scripts/linux
    .github/workflows/linux-ci.yml
)
git add -- "${PATHS[@]}"

if git diff --cached --quiet; then
    echo "Nothing to commit (the groundwork is already committed)."
else
    git commit -m "linux: groundwork for the raw Linux port

- docs/LINUX_PORT_PLAN.md: phased plan, Win32 -> Linux mapping, risks
- src/platform/*.h: OS-neutral interfaces (capture, audio, windows, hotkeys, process, paths)
- src/platform/linux/: session detection, XDG paths, 'not implemented' backend stubs,
  homrec CLI (doctor / paths / start)
- cmake/LinuxPort.cmake + guard in CMakeLists.txt: non-Windows builds hand over to it
- scripts/linux/*: build, test, dependency installer
- .github/workflows/linux-ci.yml: build + portable unit tests on ubuntu"
fi

if [ "${1:-}" = "--push" ]; then
    git push -u origin "$BRANCH"
fi

echo "Done. You are on '$BRANCH'."
