#!/usr/bin/env bash
# Append one explanation page to the long-lived pr-diff-explained branch and
# print the URLs that point at it.
#
#   bash .claude/skills/explain-diff-html/scripts/publish.sh <file.html> [pr-number]
#
# A directory sitting next to the page and named after it — page.html and
# page/ — is published with it, which is where the screenshots go. The page
# then reaches them at `page/after-phone.png`, a relative path that resolves
# the same under the rendered link, the raw one and the permalink, because all
# three address the one commit written here.
#
# The branch is an orphan: no history in common with main, nothing on it but
# these pages. It is a collection that stays open, never a pull request, so
# `git log origin/pr-diff-explained` is the only index it needs.
#
# Nothing here goes near the working tree. The commit is built with plumbing
# against a temporary index — hash-object, write-tree, commit-tree — because
# this runs in the middle of the very change it is explaining, and a checkout
# or a stash at that moment loses somebody's afternoon.
set -uo pipefail

branch=pr-diff-explained
src=${1:-}
pr=${2:-}

[[ -n $src ]] || { echo "usage: publish.sh <file.html> [pr-number]" >&2; exit 2; }
[[ -f $src ]] || { echo "no such file: $src" >&2; exit 2; }

repo_root=$(git rev-parse --show-toplevel) || exit 1
cd "$repo_root"

name=$(basename "$src")
# The date prefix is what keeps the branch listing time-sorted; a page that
# arrives without one sorts into the middle of last year and is never seen.
[[ $name =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}- ]] || {
  echo "filename must start with YYYY-MM-DD-: $name" >&2; exit 2
}

# Fetch before building the commit, and parent it to the *remote* tip. A fresh
# container has never seen this branch: without this it would start a second
# orphan history, and the push would then demand a force to overwrite pages
# somebody else already published.
parent=""
if git ls-remote --exit-code --heads origin "$branch" >/dev/null 2>&1; then
  git fetch --quiet origin "+refs/heads/$branch:refs/remotes/origin/$branch" || exit 1
  parent=$(git rev-parse "refs/remotes/origin/$branch")
fi

# Screenshots ride along in a directory named after the page. Naming it after
# the page rather than a shared `assets/` is what makes two pages published on
# the same day unable to overwrite each other's shots.
stem=${name%.html}
assets_dir=$(dirname "$src")/$stem

# The name, not the file: git rejects a zero-byte index ("index file smaller
# than expected"), and it writes its own the moment it is handed a path.
tmp_index=$(mktemp) && rm -f "$tmp_index"
trap 'rm -f "$tmp_index"' EXIT

blob=$(git hash-object -w -- "$src") || exit 1

# A file of the same name is replaced rather than duplicated, so re-running
# for the same PR on the same day publishes a correction, not a second page.
export GIT_INDEX_FILE="$tmp_index"
[[ -n $parent ]] && { git read-tree "$parent" || exit 1; }
git update-index --add --cacheinfo "100644,$blob,$name" || exit 1

# Clear the page's old shots before adding the new ones. Adding on top would
# leave a screenshot of a layout nobody ships in the tree, still reachable at
# the path the previous version of the page pointed at.
if [[ -n $parent ]]; then
  while IFS= read -r -d '' old; do
    git update-index --force-remove -- "$old" || exit 1
  done < <(git ls-files -z -- "$stem/")
fi

assets=0
bytes=0
if [[ -d $assets_dir ]]; then
  while IFS= read -r -d '' shot; do
    rel=${shot#"$assets_dir"/}
    # Images only: the directory is assembled by hand next to a draft in /tmp,
    # and a stray log or .DS_Store published beside the page is forever.
    case ${rel,,} in
      *.png|*.jpg|*.jpeg|*.gif|*.webp|*.avif|*.svg) ;;
      *) echo "skipping non-image asset: $rel" >&2; continue ;;
    esac
    shot_blob=$(git hash-object -w -- "$shot") || exit 1
    git update-index --add --cacheinfo "100644,$shot_blob,$stem/$rel" || exit 1
    assets=$((assets + 1))
    bytes=$((bytes + $(wc -c <"$shot")))
  done < <(find "$assets_dir" -type f -print0 | sort -z)
fi

tree=$(git write-tree) || exit 1
unset GIT_INDEX_FILE

# The branch keeps every page ever published, so weight added here is never
# reclaimed. Phone-width PNGs are tens of kilobytes; a megabyte of them means
# full-page desktop shots that the reader will scroll past on a phone anyway.
(( bytes > 4 * 1024 * 1024 )) && echo "warning: ${bytes} bytes of screenshots — prefer phone-width shots" >&2

subject="explain${pr:+ PR #$pr}: $name"
commit=$(git commit-tree "$tree" ${parent:+-p "$parent"} -m "$subject") || exit 1
git update-ref "refs/heads/$branch" "$commit" || exit 1

# Retries are for the network, not for a rejected push: parenting to the
# remote tip above means this is a fast-forward or it is a real conflict.
delay=2
for attempt in 1 2 3 4 5; do
  git push -u origin "$branch" && break
  [[ $attempt -eq 5 ]] && { echo "push failed after 5 attempts" >&2; exit 1; }
  sleep "$delay"; delay=$((delay * 2))
done

origin_url=$(git remote get-url origin)
echo "sha=$commit"
echo "file=$name"
echo "assets=$assets"

# Only GitHub has these URL shapes. Somewhere else, say so rather than print a
# link that looks right and 404s from a pull request comment.
if [[ $origin_url != *github.com* ]]; then
  echo "origin is not GitHub, no permalink: $origin_url" >&2
  exit 0
fi
slug=$(sed -E 's#^git@github\.com:#https://github.com/#; s#\.git$##; s#^https://github\.com/##' <<<"$origin_url")

echo "permalink=https://github.com/$slug/blob/$commit/$name"
echo "raw=https://raw.githubusercontent.com/$slug/$commit/$name"
# GitHub serves .html as source, never as a page, so the rendered link has to
# come from a third party. It has to be one addressed by *path*: a viewer that
# takes the page as a query string carries a whole URL inside its own, and a
# GitHub comment pipeline reads that shape as an open redirect and wraps it in
# backticks — posted, and tappable by nobody. githack serves the raw file with
# a text/html content type instead, and reaches public repositories only.
#
# rawcdn, not raw: the raw host is the development one and is rate limited,
# and this URL is going somewhere it will be read months from now.
echo "preview=https://rawcdn.githack.com/$slug/$commit/$name"
