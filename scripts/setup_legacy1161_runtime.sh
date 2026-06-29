#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
official_dir="$repo_root/runtime/downloads/blizzard-official"
desktop_patch_dir="${HOME:-}/Desktop/starcraft1-1.16.1"
patch_dir="${STARCRAFT_API_BW1161_PATCH_DIR:-}"
if [[ -z "$patch_dir" ]]; then
  if [[ -f "$official_dir/SC-1161.exe" && -f "$official_dir/BW-1161.exe" ]]; then
    patch_dir="$official_dir"
  elif [[ -n "${HOME:-}" && -f "$desktop_patch_dir/SC-1161.exe" && -f "$desktop_patch_dir/BW-1161.exe" ]]; then
    patch_dir="$desktop_patch_dir"
  else
    patch_dir="$official_dir"
  fi
fi
install_dir="${STARCRAFT_API_BW1161_DIR:-$repo_root/runtime/bw1161}"
base_dir="${STARCRAFT_API_BW_BASE_DIR:-}"
installer="${STARCRAFT_API_BW_INSTALLER:-}"
wine_bin="${STARCRAFT_API_WINE:-}"
wine_prefix="${STARCRAFT_API_WINEPREFIX:-$patch_dir/wineprefix}"
min_free_mb="${STARCRAFT_API_BW1161_MIN_FREE_MB:-4096}"
sc1161_sha256="755b4dbe3f8a928831b19bfa975445885b8c1760ffa4e5a795d37e7f02e6c31e"
bw1161_sha256="96890f59b664eb54dbb3be634f2045e70a4a757e87b405ec4aeeb69d50fb7bb1"
sc1161_size=10696135
bw1161_size=26497843

fail() {
  printf 'legacy1161.setup.error=%s\n' "$*" >&2
  exit 1
}

info() {
  printf '%s\n' "$*"
}

find_wine() {
  if [[ -n "$wine_bin" ]]; then
    command -v "$wine_bin" 2>/dev/null || {
      [[ -x "$wine_bin" ]] && printf '%s\n' "$wine_bin"
    }
    return 0
  fi

  for candidate in \
    /opt/homebrew/bin/wine \
    /usr/local/bin/wine \
    /usr/bin/wine \
    "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine" \
    "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"; do
    [[ -x "$candidate" ]] && {
      printf '%s\n' "$candidate"
      return 0
    }
  done

  command -v wine 2>/dev/null || true
}

free_mb() {
  df -m "$repo_root" | awk 'NR == 2 { print $4 }'
}

lower_basename() {
  basename "$1" | tr '[:upper:]' '[:lower:]'
}

file_size() {
  local size
  if size="$(stat -f '%z' "$1" 2>/dev/null)" && [[ "$size" =~ ^[0-9]+$ ]]; then
    printf '%s\n' "$size"
    return 0
  fi
  if size="$(stat -c '%s' "$1" 2>/dev/null)" && [[ "$size" =~ ^[0-9]+$ ]]; then
    printf '%s\n' "$size"
    return 0
  fi
  fail "unable to stat file size: $1"
}

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{ print $1 }'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{ print $1 }'
  else
    fail "shasum or sha256sum is required to verify downloaded patchers"
  fi
}

verify_pe32_i386_file() {
  local path="$1"
  command -v file >/dev/null 2>&1 || fail "file is required to verify Windows executable type"
  file "$path" | grep -Eq 'PE32 executable.*Intel 80386' || {
    fail "unsafe executable type for $path; expected PE32 Intel 80386"
  }
}

verify_official_patch_artifact() {
  local path="$1"
  local name expected_sha expected_size actual_sha actual_size
  name="$(lower_basename "$path")"
  case "$name" in
    sc-1161.exe)
      expected_sha="$sc1161_sha256"
      expected_size="$sc1161_size"
      ;;
    bw-1161.exe)
      expected_sha="$bw1161_sha256"
      expected_size="$bw1161_size"
      ;;
    *)
      fail "unexpected legacy patcher name: $path"
      ;;
  esac

  verify_pe32_i386_file "$path"
  actual_size="$(file_size "$path")"
  if [[ "$actual_size" != "$expected_size" ]]; then
    fail "patcher size mismatch for $path: got $actual_size expected $expected_size"
  fi
  actual_sha="$(sha256_file "$path")"
  if [[ "$actual_sha" != "$expected_sha" ]]; then
    fail "patcher sha256 mismatch for $path: got $actual_sha expected $expected_sha"
  fi
  info "legacy1161.setup.patch_verified=$(basename "$path")"
}

download_if_missing() {
  local url="$1"
  local out="$2"
  [[ -f "$out" ]] && return 0
  command -v curl >/dev/null 2>&1 || fail "curl is required to download official Blizzard artifacts"
  mkdir -p "$(dirname "$out")"
  info "legacy1161.setup.download=$url"
  local tmp="${out}.tmp.$$"
  curl --fail --location --show-error --silent "$url" -o "$tmp"
  mv "$tmp" "$out"
}

ensure_official_patch_artifacts() {
  download_if_missing \
    "http://ftp.blizzard.com/pub/starcraft/patches/PC/SC-1161.exe" \
    "$patch_dir/SC-1161.exe"
  download_if_missing \
    "http://ftp.blizzard.com/pub/broodwar/patches/PC/BW-1161.exe" \
    "$patch_dir/BW-1161.exe"
  verify_official_patch_artifact "$patch_dir/SC-1161.exe"
  verify_official_patch_artifact "$patch_dir/BW-1161.exe"
}

find_ci_file() {
  local root="$1"
  local name="$2"
  find "$root" -maxdepth 1 -iname "$name" -type f -print -quit 2>/dev/null
}

has_legacy_executable() {
  local root="$1"
  [[ -n "$(find_ci_file "$root" 'StarCraft.exe')" ]] && return 0
  [[ -n "$(find_ci_file "$root" 'Brood War.exe')" ]] && return 0
  [[ -n "$(find_ci_file "$root" 'BroodWar.exe')" ]] && return 0
  return 1
}

has_required_mpqs() {
  local root="$1"
  [[ -n "$(find_ci_file "$root" 'StarDat.mpq')" ]] || return 1
  [[ -n "$(find_ci_file "$root" 'BrooDat.mpq')" ]] || return 1
  return 0
}

base_install_ready() {
  local root="$1"
  [[ -d "$root" ]] || return 1
  has_legacy_executable "$root" || return 1
  has_required_mpqs "$root" || return 1
  return 0
}

describe_base_install_status() {
  local root="$1"
  if [[ ! -d "$root" ]]; then
    printf 'missing'
  elif ! has_legacy_executable "$root"; then
    printf 'missing-executable'
  elif ! has_required_mpqs "$root"; then
    printf 'missing-required-mpq'
  else
    printf 'ready'
  fi
}

validate_base_install_dir() {
  local root="$1"
  local status
  status="$(describe_base_install_status "$root")"
  [[ "$status" == "ready" ]] || fail "incomplete StarCraft Brood War base install at $root: $status"
}

validate_installer_file() {
  [[ -n "$installer" ]] || return 0
  [[ -f "$installer" ]] || fail "STARCRAFT_API_BW_INSTALLER does not exist: $installer"
  case "$(lower_basename "$installer")" in
    sc-1161.exe|bw-1161.exe)
      fail "STARCRAFT_API_BW_INSTALLER points to a patcher, not a base game installer: $installer"
      ;;
    battle.net-setup.exe)
      fail "STARCRAFT_API_BW_INSTALLER points to Battle.net setup, not a legacy 1.16.1 base game installer: $installer"
      ;;
  esac
  verify_pe32_i386_file "$installer"
}

copy_base_install() {
  [[ -n "$base_dir" ]] || return 0
  [[ -d "$base_dir" ]] || fail "STARCRAFT_API_BW_BASE_DIR does not exist: $base_dir"
  validate_base_install_dir "$base_dir"
  mkdir -p "$install_dir"
  rsync -a --delete "$base_dir"/ "$install_dir"/
}

find_installed_starcraft() {
  local roots=(
    "$install_dir"
    "$wine_prefix/drive_c/Program Files/StarCraft"
    "$wine_prefix/drive_c/Program Files (x86)/StarCraft"
    "$wine_prefix/drive_c/StarCraft"
  )
  local root
  for root in "${roots[@]}"; do
    [[ -f "$root/StarCraft.exe" || -f "$root/Brood War.exe" ]] && {
      printf '%s\n' "$root"
      return 0
    }
  done

  if [[ -d "$wine_prefix/drive_c" ]]; then
    find "$wine_prefix/drive_c" -maxdepth 5 \( -iname 'StarCraft.exe' -o -iname 'Brood War.exe' \) -print -quit |
      sed 's#/[^/]*$##'
  fi
}

run_installer_if_requested() {
  [[ -n "$installer" ]] || return 0
  [[ -n "$wine_bin" ]] || fail "Wine is not installed; run: brew install --cask wine-stable"
  mkdir -p "$wine_prefix"
  info "legacy1161.setup.installer=$installer"
  WINEPREFIX="$wine_prefix" "$wine_bin" "$installer"

  local discovered
  discovered="$(find_installed_starcraft || true)"
  if [[ -n "$discovered" ]]; then
    install_dir="$discovered"
    info "legacy1161.setup.installed_dir=$install_dir"
  fi
}

run_patch_if_needed() {
  local patch="$1"
  [[ -f "$patch" ]] || fail "missing patch executable: $patch"
  verify_official_patch_artifact "$patch"
  [[ -n "$wine_bin" ]] || fail "Wine is not installed; run: brew install --cask wine-stable"
  mkdir -p "$wine_prefix"
  (
    cd "$install_dir"
    WINEPREFIX="$wine_prefix" "$wine_bin" "$patch"
  )
}

info "legacy1161.setup.repo=$repo_root"
info "legacy1161.setup.patch_dir=$patch_dir"
info "legacy1161.setup.install_dir=$install_dir"
info "legacy1161.setup.wineprefix=$wine_prefix"

free="$(free_mb)"
info "legacy1161.setup.free_mb=$free"
if [[ "$free" -lt "$min_free_mb" ]]; then
  fail "free disk space is ${free}MB, need at least ${min_free_mb}MB"
fi

ensure_official_patch_artifacts

[[ -f "$patch_dir/SC-1161.exe" ]] || fail "missing SC-1161.exe in $patch_dir"
[[ -f "$patch_dir/BW-1161.exe" ]] || fail "missing BW-1161.exe in $patch_dir"

validate_installer_file
copy_base_install

if [[ -z "$installer" ]] && ! base_install_ready "$install_dir"; then
  info "legacy1161.setup.base_install=$(describe_base_install_status "$install_dir")"
  info "legacy1161.setup.next=set STARCRAFT_API_BW_BASE_DIR to a Windows StarCraft + Brood War install folder containing StarCraft.exe, StarDat.mpq, and BrooDat.mpq, or set STARCRAFT_API_BW_INSTALLER to a Windows base installer"
  exit 2
fi

wine_bin="$(find_wine)"
if [[ -z "$wine_bin" ]]; then
  info "legacy1161.setup.wine=missing"
  if [[ -n "$installer" ]]; then
    info "legacy1161.setup.next=install Wine, then rerun with STARCRAFT_API_BW_INSTALLER=$installer"
  else
    info "legacy1161.setup.next=install Wine to patch and launch the existing StarCraft base install"
  fi
  exit 3
else
  info "legacy1161.setup.wine=$wine_bin"
fi

run_installer_if_requested

if ! base_install_ready "$install_dir"; then
  info "legacy1161.setup.base_install=$(describe_base_install_status "$install_dir")"
  info "legacy1161.setup.next=verify that STARCRAFT_API_BW_INSTALLER installs StarCraft.exe, StarDat.mpq, and BrooDat.mpq"
  exit 2
fi

run_patch_if_needed "$patch_dir/SC-1161.exe"
run_patch_if_needed "$patch_dir/BW-1161.exe"

legacy_exe="$install_dir/StarCraft.exe"
if [[ ! -f "$legacy_exe" && -f "$install_dir/Brood War.exe" ]]; then
  legacy_exe="$install_dir/Brood War.exe"
fi

"$repo_root/build/starcraft-runtime-legacy1161-setup" \
  --executable "$legacy_exe" \
  --wine "$wine_bin" \
  --wine-prefix "$wine_prefix" \
  --write-env "$repo_root/runtime/bw1161.env" \
  --write-manifest "$repo_root/runtime/bw1161.manifest" \
  --require-launchable

info "legacy1161.setup.complete=true"
