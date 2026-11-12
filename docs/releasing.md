<!--
SPDX-FileCopyrightText: 2026 Karten contributors
SPDX-License-Identifier: CC0-1.0
-->

# Releasing

Karten is distributed as a Flatpak from its own repository on GitHub Pages.
Publishing a release on GitHub does everything; this page says what to do
before that, what happens after, and what the set-up consists of.

## Making a release

1. **Set the version** in `CMakeLists.txt` (`project(karten VERSION x.y.z …)`).
   The About page, the User-Agent and the Flatpak take it from there.
2. **Describe it** in `io.github.n4n0gh.karten.metainfo.xml`: add a `<release>`
   with the version, today's date and a short description at the top of
   `<releases>`. Discover shows this to users as what is new.
3. **Check** that it builds and the tests pass (`ctest --test-dir build`; the
   `appstreamtest` validates the metainfo file).
4. **Commit and push** to `main`.
5. **Publish a release** on GitHub for a new tag named `v` plus the version:

   ```sh
   gh release create v0.1.2 --target main --title "Karten 0.1.2" --notes "…"
   ```

   or with *Draft a new release* on the web site. The tag has to start with
   `v`: only such tags may publish to GitHub Pages (see below).

That is all. A few minutes later the new version is offered to everybody who
has Karten installed.

## What happens then

Publishing the release starts `.github/workflows/flatpak.yml`:

| Step | What it does |
| --- | --- |
| Build | `flatpak-builder` builds the manifest in a container with the KDE runtime, on the Flatpak branch `stable`. KOpeningHours and MapLibre come out of the cache unless the manifest or the patch changed. |
| Prepare | `flatpak/make-site.sh` signs everything, writes the repository's summary, the files to install from and a small page, and builds `karten.flatpak`. |
| Attach | `karten.flatpak` is attached to the release. |
| Publish | The repository is deployed to GitHub Pages, replacing the previous one. |

It takes about three minutes, or about eleven when MapLibre has to be built.

Each publication contains the new version alone, not the history. That is all
an update needs, and it keeps the site small.

### Checking that it worked

```sh
gh run list --workflow flatpak.yml --limit 1                 # the run succeeded
gh release view v0.1.2 --json assets -q '.assets[].name'     # karten.flatpak is attached
flatpak remote-ls --user --updates | grep karten             # an installed copy sees the update
```

## What users have

- **Installing**: `flatpak install --user https://n4n0gh.github.io/Karten/karten.flatpakref`,
  the *Install Karten* link on <https://n4n0gh.github.io/Karten/>, or the
  `karten.flatpak` of a release.
- All three add the repository as a remote (`karten-origin`) with signature
  checking, so Karten is updated with the user's other applications, in
  Discover or with `flatpak update`.
- The published site: `repo/` (the Flatpak repository), `karten.flatpakref`
  (install the application), `karten.flatpakrepo` (add the repository only)
  and `index.html`.

## Building without releasing

The workflow can be started by hand: *Actions → Flatpak → Run workflow*, or

```sh
gh workflow run flatpak.yml --ref main                   # build only
gh workflow run flatpak.yml --ref main -f publish=true   # build and publish
```

Without `publish` the bundle is an artifact of the run (`karten-flatpak`) and
nothing users see changes. **With `publish`, whatever is on that branch goes
out to every installed copy as an update**, release or not. It is meant for
republishing after a fix to the workflow or the publishing script.

To try the publishing script on a locally built repository:

```sh
flatpak-builder --user --force-clean --repo=repo build-flatpak flatpak/io.github.n4n0gh.karten.yml
flatpak/make-site.sh repo site http://127.0.0.1:8000 master
python3 -m http.server 8000 --bind 127.0.0.1 --directory site
flatpak remote-add --user --no-gpg-verify karten-test http://127.0.0.1:8000/repo/
flatpak remote-ls --user karten-test
```

(`master` is the branch a plain `flatpak-builder` run uses; the workflow
builds `stable`.)

## The set-up

Things that exist outside the source tree and would have to be recreated for
a new home of the repository.

### GitHub Pages

- Pages is enabled for the repository with *GitHub Actions* as its source.
- The `github-pages` environment allows deployments from the branch `main`
  and from tags matching `v*`. A release runs the workflow for its tag, and
  without that rule the deployment is refused.
- The address is `https://n4n0gh.github.io/Karten`. It is in
  `.github/workflows/flatpak.yml` (`SITE_URL`), in the README, and **in every
  installed copy**, which updates from it. Changing it strands existing
  installations; they would have to install again from the new address.

### The signing key

Everything published is signed with a GPG key made for this purpose
(`Karten Flatpak repository`, fingerprint
`8A20 D085 DD9E 0F23 4636 CA1C 9FB2 99C7 B63B 7F0E`).

- Its secret half is the repository secret `FLATPAK_GPG_PRIVATE_KEY`:
  ASCII-armoured, without a passphrase.
- Its public half is in `karten.flatpakref`, `karten.flatpakrepo` and the
  bundle, and from there in every installed copy.
- GitHub does not give secrets back. **Whoever maintains the releases has to
  keep a copy of the secret key.** If it is lost, a new key has to be made,
  and every user has to remove the remote and install again, because their
  copies only trust the old one.
- To put a key in place: `gh secret set FLATPAK_GPG_PRIVATE_KEY < secret-key.asc`.

Without the secret the workflow still runs, and publishes an unsigned
repository. Clients that already check signatures then refuse the update, so
that is only an option for a repository nobody has installed from yet.

`make-site.sh` signs the application, its translations (`.Locale`) and debug
symbols (`.Debug`), and makes the description for software centres be written
anew so that it is signed as well. A part that is left unsigned is refused by
clients with "GPG verification enabled, but no signatures found".

## When something changes

- **A newer KDE runtime**: change `runtime-version` in the manifest and the
  container image in the workflow together (`kde-6.11` and so on). Users get
  the new runtime with the update.
- **A newer KOpeningHours or MapLibre**: change the address and the checksum
  in the manifest. For MapLibre use the release's source archive, not the git
  repository, which with its submodules and their history is several
  gigabytes. Check whether `maplibre-qt-location-private.patch` is still
  needed and still applies.
- **The build cache** is keyed on the manifest and the patch. Changing either
  rebuilds everything once; changing the publishing script does not.
- **A broken release**: fix it and publish a new version. To take a release
  back in the meantime, publish the previous one again from its tag:
  `gh workflow run flatpak.yml --ref v0.1.1 -f publish=true`. Installed
  copies follow whatever is published. Only `main` and `v*` tags may publish,
  and the tag has to be 0.1.1 or later, as the workflow at an older tag
  cannot do this.
- **Replacing the file of a release**:
  `gh release upload v0.1.2 karten.flatpak --clobber`.
