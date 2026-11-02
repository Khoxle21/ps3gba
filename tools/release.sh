#!/bin/sh
# Publishes the current version on GitHub: builds the release package, tags the commit vX.Y.Z,
# creates the release with CHANGELOG.md's entry as notes and ps3gba-vX.Y.Z.pkg attached.
# Versions before 1.0.0 are marked pre-release. Every version bump gets a release.
#   tools/release.sh            (needs gh logged into the repo owner's account)
# Refuses unless the tree is clean, main is pushed and the version has no release yet.
set -e
cd "$(dirname "$0")/.."
v=$(sed -n 's/^## \([0-9][0-9.]*\) .*/\1/p' CHANGELOG.md | head -n 1)
[ -n "$v" ] || { echo "no version in CHANGELOG.md" >&2; exit 1; }
tag="v$v"
repo=$(git remote get-url origin | sed -E 's#^.*[:/]([^/]+/[^/]+)$#\1#; s#\.git$##')
[ -z "$(git status --porcelain)" ] || { echo "uncommitted changes: commit them first" >&2; exit 1; }
git fetch -q origin main
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "HEAD is not origin/main: push first" >&2; exit 1; }
if git rev-parse -q --verify "refs/tags/$tag" >/dev/null || git ls-remote --exit-code --tags origin "$tag" >/dev/null 2>&1; then
	echo "$tag exists already: bump the version in CHANGELOG.md" >&2; exit 1
fi
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in: gh auth login" >&2; exit 1; }

# The release build (no dev tools), named after the version.
rm -f ps3gba.pkg ps3gba.gnpdrm.pkg
./build.sh pkg
if strings ps3gba.elf | grep -q 'dev: screenshot saved'; then echo "not a release build" >&2; exit 1; fi
strings ps3gba.elf | grep -q "V$v" || { echo "the build doesn't show V$v" >&2; exit 1; }
mkdir -p dist
pkg="dist/ps3gba-$tag.pkg"
cp ps3gba.gnpdrm.pkg "$pkg"

# Notes: the version's CHANGELOG entry, then how to install.
notes=$(mktemp)
awk -v v="$v" '/^## /{on = index($0, "## " v " ") == 1; next} on' CHANGELOG.md > "$notes"
cat >> "$notes" <<'EOF'

**Install:** copy `ps3gba-vX.Y.Z.pkg` to a USB stick or to `/dev_hdd0/packages/` on a PS3 with
CFW or PS3HEN, then Package Manager > Install Package Files. Put your own game dumps in
`/dev_hdd0/ps3gba/roms/` (FTP) or add them from the phone page. See the README; every
version's changes are in CHANGELOG.md.
EOF
sed -i '' "s/ps3gba-vX.Y.Z.pkg/ps3gba-$tag.pkg/" "$notes" 2>/dev/null || sed -i "s/ps3gba-vX.Y.Z.pkg/ps3gba-$tag.pkg/" "$notes"

git tag -a "$tag" -m "ps3gba $v"
git push -q origin "$tag"
pre=""
case "$v" in 0.*) pre="--prerelease" ;; esac
gh release create "$tag" "$pkg" --repo "$repo" --title "ps3gba $v" --notes-file "$notes" $pre
rm -f "$notes"
