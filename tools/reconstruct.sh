#!/usr/bin/env bash
# Rebuilds the full working tree this snapshot was taken from, next to each other in OUT:
#   OUT/UniXNU         SadNutWall/UniXNU at its base, the local commits (unixnu/commits/UniXNU), every submodule at its base with
#                      its local commits, the uncommitted diffs (unixnu/diffs) and the kernel patches (unixnu/patches)
#   OUT/device         this snapshot's device/ (already the final source)
#   OUT/device-ios17   this snapshot's ios17/ (its symbolic links point back to ../device)
# usage: tools/reconstruct.sh OUT
# Needs git and network access to github.com. Nothing from Apple is fetched here.
set -euo pipefail
SNAP=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${1:?usage: tools/reconstruct.sh OUT}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
export GIT_AUTHOR_NAME=fuxi GIT_AUTHOR_EMAIL=fuxi@users.noreply.github.com GIT_COMMITTER_NAME=fuxi GIT_COMMITTER_EMAIL=fuxi@users.noreply.github.com
BASES="$SNAP/unixnu/commits/BASES.txt"

base_of() { awk -v m="$1" '$1==m{print $2}' "$BASES"; }
url_of()  { awk -v m="$1" '$1==m{print $3}' "$BASES"; }

# Applies the patches of one directory: mbox patches with git am, a combined diff (no "From:" header) with git apply and a commit.
apply_dir() {
  local repo=$1 dir=$2 p
  for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    if grep -q '^From: ' "$p"; then
      git -C "$repo" am --3way --keep-cr -q "$p" || { echo "git am failed: $p" >&2; git -C "$repo" am --abort || true; return 1; }
    else
      git -C "$repo" apply --index --binary "$p"
      git -C "$repo" commit -q -m "$(grep -m1 '^Subject: ' "$p" | sed 's/^Subject: \(\[PATCH\] \)\{0,1\}//')"
    fi
    echo "  applied $(basename "$p")"
  done
}

echo "== UniXNU"
if [ ! -d "$OUT/UniXNU/.git" ]; then
  git clone -q "$(url_of UniXNU)" "$OUT/UniXNU"
fi
git -C "$OUT/UniXNU" checkout -q "$(base_of UniXNU)"
git -C "$OUT/UniXNU" submodule update -q --init
apply_dir "$OUT/UniXNU" "$SNAP/unixnu/commits/UniXNU"

for d in "$SNAP"/unixnu/commits/*/; do
  m=$(basename "$d")
  case "$m" in UniXNU|device) continue ;; esac
  path=${m/_//}
  echo "== $path"
  sub="$OUT/UniXNU/$path"
  [ -d "$sub/.git" ] || [ -f "$sub/.git" ] || git clone -q "$(url_of "$m")" "$sub"
  git -C "$sub" fetch -q origin "$(base_of "$m")" 2>/dev/null || git -C "$sub" fetch -q origin
  git -C "$sub" checkout -q "$(base_of "$m")"
  apply_dir "$sub" "$d"
done

echo "== uncommitted diffs"
for f in "$SNAP"/unixnu/diffs/*.diff; do
  name=$(basename "$f" .diff)
  case "$name" in UniXNU) target="$OUT/UniXNU" ;; *) target="$OUT/UniXNU/${name/_//}" ;; esac
  [ -s "$f" ] && git -C "$target" apply --binary "$f" && echo "  $name"
done

echo "== kernel patches"
mkdir -p "$OUT/UniXNU/patches/xnu-10063-arm64e"
cp -a "$SNAP/unixnu/patches/xnu-10063-arm64e/." "$OUT/UniXNU/patches/xnu-10063-arm64e/"

echo "== device and device-ios17"
rm -rf "$OUT/device" "$OUT/device-ios17"
cp -a "$SNAP/device" "$OUT/device"
cp -a "$SNAP/ios17" "$OUT/device-ios17"
cp -a "$SNAP/host/." "$OUT/"

echo "== done: $OUT"
git -C "$OUT/UniXNU" log --oneline -1
