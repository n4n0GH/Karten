#!/bin/sh
# SPDX-FileCopyrightText: 2026 Karten contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turns the repository that flatpak-builder exported into a web site that
# Flatpak can install and update Karten from, and makes a bundle that
# remembers where it came from.
#
#   flatpak/make-site.sh REPO SITE URL [BRANCH]
#
# REPO    the exported repository (flatpak-builder --repo)
# SITE    directory to create; to be served at URL
# URL     where SITE will be reachable, without a slash at the end
# BRANCH  branch of the application in REPO (default: stable)
#
# With the secret key of a GPG key in the environment variable
# GPG_PRIVATE_KEY, the repository is signed and the files for installing
# carry the public key, so that Flatpak verifies what it downloads.

set -eu

repo=$1
site=$2
url=$3
branch=${4:-stable}
app=io.github.n4n0gh.karten
runtime_repo=https://dl.flathub.org/repo/flathub.flatpakrepo

sign=
bundle_key=
key_line=
if [ -n "${GPG_PRIVATE_KEY:-}" ]; then
    GNUPGHOME=$(mktemp -d)
    export GNUPGHOME
    trap 'rm -rf "$GNUPGHOME"' EXIT
    printf '%s\n' "$GPG_PRIVATE_KEY" | gpg --batch --quiet --import
    key_id=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1 == "sec" { print $5; exit }')
    sign="--gpg-sign=$key_id --gpg-homedir=$GNUPGHOME"
    gpg --batch --export "$key_id" > "$GNUPGHOME/public.gpg"
    key_line="GPGKey=$(base64 -w 0 < "$GNUPGHOME/public.gpg")"
    bundle_key="--gpg-keys=$GNUPGHOME/public.gpg"
    # shellcheck disable=SC2086
    flatpak build-sign "$repo" "$app" "$branch" $sign
fi

# The summary is what clients read to learn what the repository offers.
# shellcheck disable=SC2086
flatpak build-update-repo --title=Karten --default-branch="$branch" --prune $sign "$repo"

rm -rf "$site"
mkdir -p "$site"
cp -a "$repo" "$site/repo"

# Installs Karten and adds the repository, for updates.
cat > "$site/karten.flatpakref" <<REF
[Flatpak Ref]
Name=$app
Branch=$branch
Title=Karten
Url=$url/repo/
RuntimeRepo=$runtime_repo
IsRuntime=false
$key_line
REF

# Adds the repository only.
cat > "$site/karten.flatpakrepo" <<REPO
[Flatpak Repo]
Title=Karten
Url=$url/repo/
Homepage=https://github.com/n4n0gh/karten
Comment=Maps and route planning that also work offline
DefaultBranch=$branch
$key_line
REPO

cat > "$site/index.html" <<HTML
<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Karten for Flatpak</title>
<style>
  body { font: 1rem/1.5 system-ui, sans-serif; max-width: 40rem; margin: 3rem auto; padding: 0 1rem; }
  code { background: #8882; padding: .1em .3em; border-radius: .2em; }
</style>
<h1>Karten</h1>
<p>Maps and route planning that also work offline, for the Plasma desktop.</p>
<p><a href="karten.flatpakref">Install Karten</a> with Discover or another software centre, or from a terminal:</p>
<p><code>flatpak install --user $url/karten.flatpakref</code></p>
<p>Installed this way, Karten is kept up to date together with your other applications.</p>
<p><a href="https://github.com/n4n0gh/karten">Source code and more about Karten</a></p>
</html>
HTML

# A single file for those who prefer a download; it updates from the same place.
# shellcheck disable=SC2086
flatpak build-bundle --repo-url="$url/repo/" --runtime-repo="$runtime_repo" $bundle_key "$repo" karten.flatpak "$app" "$branch"

echo "Site for $url written to $site"
