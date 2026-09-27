#!/bin/sh
# pmux installer: downloads the static pmux binary for this machine from a
# GitHub release, verifies it against SHA256SUMS and installs it as `pmux`.
#
#   curl -fsSL https://raw.githubusercontent.com/knucklehead96/pmux/main/install.sh | sh
#
# Environment variables:
#   PMUX_VERSION      version to install, e.g. 0.1.0 or v0.1.0 (default: latest)
#   PMUX_INSTALL_DIR  where to install (default: /usr/local/bin as root,
#                     otherwise $HOME/.local/bin)
#   PMUX_REPO         GitHub repository to download from (default: knucklehead96/pmux)
#   PMUX_BASE_URL     release download base URL, for testing
#                     (default: https://github.com/$PMUX_REPO/releases/download)
set -eu

PMUX_REPO=${PMUX_REPO:-knucklehead96/pmux}
PMUX_BASE_URL=${PMUX_BASE_URL:-https://github.com/$PMUX_REPO/releases/download}
SOURCE_URL=https://github.com/knucklehead96/pmux#build-from-source

say() { printf 'pmux-install: %s\n' "$*"; }
warn() { printf 'pmux-install: warning: %s\n' "$*" >&2; }
die() {
  printf 'pmux-install: error: %s\n' "$*" >&2
  exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }

detect_arch() {
  [ "$(uname -s)" = Linux ] || die "pmux supports Linux only"
  m=$(uname -m)
  case $m in
    x86_64 | amd64) arch=x86_64 ;;
    aarch64 | arm64) arch=aarch64 ;;
    *) die "unsupported architecture '$m' (supported: x86_64, aarch64); build from source: $SOURCE_URL" ;;
  esac
}

detect_downloader() {
  if have curl; then
    downloader=curl
  elif have wget; then
    downloader=wget
  else
    die "curl or wget is required"
  fi
}

# download <url> <file>: fails (non-zero) on HTTP errors such as 404.
download() {
  if [ "$downloader" = curl ]; then
    curl -fsSL -o "$2" "$1"
  else
    wget -qO "$2" "$1"
  fi
}

# Resolve the latest tag from the redirect of /releases/latest, which avoids
# the rate-limited GitHub API.
latest_tag() {
  url=https://github.com/$PMUX_REPO/releases/latest
  if [ "$downloader" = curl ]; then
    final=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "$url") || final=
  else
    final=$(wget --max-redirect=0 -S -O /dev/null "$url" 2>&1 |
      tr -d '\r' | sed -n 's/^ *[Ll]ocation: *\([^ ]*\).*/\1/p' | tail -n 1) || final=
  fi
  tag=${final##*/}
  case $final in
    */releases/tag/?*) printf '%s\n' "$tag" ;;
    *) die "could not determine the latest pmux release from $url; set PMUX_VERSION" ;;
  esac
}

verify_checksum() {  # verify_checksum <dir> <asset>
  line=$(grep " \*\{0,1\}$2\$" "$1/SHA256SUMS" || true)
  [ -n "$line" ] || die "$2 is not listed in SHA256SUMS"
  want=${line%% *}
  if have sha256sum; then
    got=$(sha256sum "$1/$2")
  elif have shasum; then
    got=$(shasum -a 256 "$1/$2")
  else
    warn "neither sha256sum nor shasum found; skipping checksum verification"
    return 0
  fi
  got=${got%% *}
  [ "$got" = "$want" ] || die "checksum mismatch for $2 (expected $want, got $got)"
  say "checksum OK"
}

default_install_dir() {
  if [ "$(id -u)" -eq 0 ]; then
    printf '%s\n' /usr/local/bin
  else
    [ -n "${HOME:-}" ] || die "HOME is not set; set PMUX_INSTALL_DIR"
    printf '%s\n' "$HOME/.local/bin"
  fi
}

main() {
  detect_arch
  detect_downloader

  if [ -n "${PMUX_VERSION:-}" ]; then
    version=${PMUX_VERSION#v}
  else
    tag=$(latest_tag)
    version=${tag#v}
  fi
  tag=v$version
  asset=pmux-$version-linux-$arch-static
  dir=${PMUX_INSTALL_DIR:-$(default_install_dir)}

  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  trap 'exit 1' HUP INT TERM

  say "downloading $asset"
  download "$PMUX_BASE_URL/$tag/$asset" "$tmp/$asset" ||
    die "no prebuilt pmux $version for linux-$arch; build from source: $SOURCE_URL"
  download "$PMUX_BASE_URL/$tag/SHA256SUMS" "$tmp/SHA256SUMS" ||
    die "could not download SHA256SUMS for pmux $version"
  verify_checksum "$tmp" "$asset"

  mkdir -p "$dir" || die "cannot create $dir"
  old=
  if [ -x "$dir/pmux" ]; then
    old=$("$dir/pmux" --version 2>/dev/null | awk '{print $2}') || old=
    [ -n "$old" ] || old=unknown
  fi

  # Copy into the target directory, then rename over the old binary: the
  # replace is atomic and works while an old pmux daemon runs from that path.
  staged=$dir/.pmux.install.$$
  cp "$tmp/$asset" "$staged" || die "cannot write to $dir (set PMUX_INSTALL_DIR)"
  chmod 755 "$staged"
  mv -f "$staged" "$dir/pmux" || {
    rm -f "$staged"
    die "cannot install $dir/pmux"
  }

  if [ "$old" = "$version" ]; then
    say "reinstalled pmux $version in $dir/pmux"
  elif [ -n "$old" ]; then
    say "upgraded $old -> $version in $dir/pmux"
    if have pgrep && pgrep -x pmux >/dev/null 2>&1; then
      say "a pmux daemon is running the old version, and the new pmux won't use it until it restarts:"
      say "  pmux --stop    (pmux --stop --force also ends its processes)"
    fi
  else
    say "installed pmux $version to $dir/pmux"
  fi

  case :${PATH:-}: in
    *:"$dir":* | *:"$dir"/:*) ;;
    *)
      say "$dir is not on your PATH; add this line to your shell profile:"
      if [ -n "${HOME:-}" ] && [ "${dir#"$HOME"/}" != "$dir" ]; then
        printf '  export PATH="$HOME/%s:$PATH"\n' "${dir#"$HOME"/}"
      else
        printf '  export PATH="%s:$PATH"\n' "$dir"
      fi
      ;;
  esac

  "$dir/pmux" --version
}

main "$@"
