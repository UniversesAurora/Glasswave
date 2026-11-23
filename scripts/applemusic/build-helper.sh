#!/usr/bin/env bash
# Reproducibly build the pinned Adam engine used by Glasswave inside WSL.
#
# The source checkout is treated as read-only. Builds extract the pinned commit
# into an isolated tree, apply scripts/applemusic/patches/*.patch in order
# there, and never modify the upstream worktree.
set -Eeuo pipefail
umask 077

readonly PINNED_COMMIT="dea30e5e56a05d742feb4f21c4f982b2bbd59c41"
readonly HYBRIS_CORE_SHA256="908b2b98134ef87ba68d083003a15caf5e05235ff4ab8df9c1e3a25bd2f770b6"
readonly HYBRIS_CORE_SIZE="383296"
readonly Q_SHA256="b5725b3be908425d028d82b0f549fea5dba396cc3f1ad98bcd5ec9a86ab0b10c"
readonly Q_SIZE="1338376"
readonly FAIRPLAY_FETCHER_SHA256="9c222e70b8f5d15cb0286834a0f644e5f636eb322d79ee2bc5fa47b74fb04553"
readonly FAIRPLAY_LOCK_SHA256="0f82b05188beac335b2fdc35cee2e625fb050836ecdd0c90f46aa5a78efbf767"
readonly SSL_DEV_VERSION="3.0.13-0ubuntu3.16"
readonly SSL_RUNTIME_VERSION="3.0.13-0ubuntu3.16"
readonly CURL_DEV_VERSION="8.5.0-2ubuntu10.16"
readonly CURL_RUNTIME_VERSION="8.5.0-2ubuntu10.16"
readonly SSL_DEV_DEB="libssl-dev_${SSL_DEV_VERSION}_amd64.deb"
readonly SSL_RUNTIME_DEB="libssl3t64_${SSL_RUNTIME_VERSION}_amd64.deb"
readonly CURL_DEV_DEB="libcurl4-openssl-dev_${CURL_DEV_VERSION}_amd64.deb"
readonly CURL_RUNTIME_DEB="libcurl4t64_${CURL_RUNTIME_VERSION}_amd64.deb"
readonly SSL_DEV_SHA256="2b4734df0475e305ac03abb0a2cc5659651ed37ef02d967a1c4033a154cf714b"
readonly SSL_RUNTIME_SHA256="219f43b1cd836a4da550938db5fda93160d269d39a4edcf1f1ce698a470db797"
readonly CURL_DEV_SHA256="371ce08af1c08bf5757467b306a040ea1d40de4e9c4acf20cabc3dde3c18a9b6"
readonly CURL_RUNTIME_SHA256="8196381b41f0078083a2a52b43f7e7ca9b0b79e5325451bf3948e4af7693cd8d"

MODE="check"
PROJECT_ROOT=""
INSTALL_GO=0
BOOTSTRAP_SYSROOT=0
STAGE_ROOT=""
PACKAGE_STAGE=""

usage() {
    cat <<'EOF'
Usage: build-helper.sh [--check|--build] [--install-go] [--bootstrap-sysroot] [--project-root PATH]

  --check       Validate source pin, patch state, runtimes, and build dependencies.
  --build       Build the DRM client, pinned FairPlay support, and Glasswave engine.
  --install-go  Install Go 1.26.0 through mise into .local/applemusic/tool.
  --bootstrap-sysroot
                Download exact Ubuntu .debs and extract local OpenSSL/libcurl
                headers and linker files into .local/applemusic/sysroot.

No apt packages are installed. The Windows setup script starts with --check.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '%s\n' "$*"
}

sha256_file() {
    sha256sum "$1" | awk '{print $1}'
}

sha256_lf_normalized_file() {
    sed 's/\r$//' "$1" | sha256sum | awk '{print $1}'
}

cleanup() {
    if [[ -n "$STAGE_ROOT" && -d "$STAGE_ROOT" && "$STAGE_ROOT" == "$LOCAL_BUILD_ROOT"/.stage-* ]]; then
        rm -rf -- "$STAGE_ROOT"
    fi
    if [[ -n "$PACKAGE_STAGE" && -d "$PACKAGE_STAGE" && "$PACKAGE_STAGE" == "$STATE_ROOT/builds"/.stage-* ]]; then
        rm -rf -- "$PACKAGE_STAGE"
    fi
}

trap cleanup EXIT

while (($#)); do
    case "$1" in
        --check)
            MODE="check"
            shift
            ;;
        --build)
            MODE="build"
            shift
            ;;
        --install-go)
            INSTALL_GO=1
            shift
            ;;
        --bootstrap-sysroot)
            BOOTSTRAP_SYSROOT=1
            shift
            ;;
        --project-root)
            [[ $# -ge 2 ]] || die "--project-root requires a path"
            PROJECT_ROOT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

if [[ -z "$PROJECT_ROOT" ]]; then
    script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
    PROJECT_ROOT=$(cd -- "$script_dir/../.." && pwd)
fi
PROJECT_ROOT=$(realpath -e -- "$PROJECT_ROOT")

readonly SOURCE_REPO="$PROJECT_ROOT/.local/applemusic/adam"
readonly PATCH_DIR="$PROJECT_ROOT/scripts/applemusic/patches"
readonly LOCAL_TOOL_ROOT="$PROJECT_ROOT/.local/applemusic/tool"
readonly LOCAL_BUILD_ROOT="$PROJECT_ROOT/.local/applemusic/build"
readonly LOCAL_MISE_DATA="$LOCAL_TOOL_ROOT/mise"
readonly LOCAL_MISE_CACHE="$LOCAL_TOOL_ROOT/mise-cache"
readonly SYSROOT_DIR="$PROJECT_ROOT/.local/applemusic/sysroot"
readonly SYSROOT_DOWNLOADS="$SYSROOT_DIR/downloads"
SYSROOT_ID=$(printf '%s\n' \
    "$SSL_DEV_SHA256" "$SSL_RUNTIME_SHA256" "$CURL_DEV_SHA256" "$CURL_RUNTIME_SHA256" |
    sha256sum | awk '{print substr($1,1,12)}')
readonly SYSROOT_ID
readonly SYSROOT_ROOT="$SYSROOT_DIR/root-$SYSROOT_ID"
readonly SYSROOT_BIN="$SYSROOT_DIR/bin-$SYSROOT_ID"
readonly SYSROOT_INCLUDE="$SYSROOT_ROOT/usr/include"
readonly SYSROOT_MULTIARCH_INCLUDE="$SYSROOT_ROOT/usr/include/x86_64-linux-gnu"
readonly SYSROOT_LIB="$SYSROOT_ROOT/usr/lib/x86_64-linux-gnu"

PROJECT_HASH=$(printf '%s\n' "$PROJECT_ROOT" | sha256sum | awk '{print substr($1,1,16)}')
readonly PROJECT_HASH
STATE_ROOT="${GLASSWAVE_APPLEMUSIC_STATE_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/glasswave-applemusic/$PROJECT_HASH}"
readonly STATE_ROOT

[[ -d "$SOURCE_REPO/.git" ]] || die "Adam source checkout is missing: $SOURCE_REPO"
command -v realpath >/dev/null 2>&1 || die "realpath is missing"
command -v sha256sum >/dev/null 2>&1 || die "sha256sum is missing"
command -v git >/dev/null 2>&1 || die "git is missing"

MISE_BIN="${MISE_BIN:-$HOME/.local/bin/mise}"
if [[ ! -x "$MISE_BIN" ]]; then
    MISE_BIN=$(command -v mise 2>/dev/null || true)
fi

ACTIVE_MISE_DATA_DIR=""
ACTIVE_MISE_CACHE_DIR=""

find_mise_go() {
    [[ -n "$MISE_BIN" && -x "$MISE_BIN" ]] || return 1

    local data_dir cache_dir version
    local -a candidates=()
    candidates+=("${MISE_DATA_DIR:-$HOME/.local/share/mise}")
    candidates+=("$LOCAL_MISE_DATA")

    for data_dir in "${candidates[@]}"; do
        [[ -d "$data_dir" ]] || continue
        if [[ "$data_dir" == "$LOCAL_MISE_DATA" ]]; then
            cache_dir="$LOCAL_MISE_CACHE"
        else
            cache_dir="${MISE_CACHE_DIR:-$HOME/.cache/mise}"
        fi
        version=$(MISE_DATA_DIR="$data_dir" MISE_CACHE_DIR="$cache_dir" \
            "$MISE_BIN" exec go@1.26.0 -- go version 2>/dev/null || true)
        if [[ "$version" == go\ version\ go1.26.0\ * ]]; then
            ACTIVE_MISE_DATA_DIR="$data_dir"
            ACTIVE_MISE_CACHE_DIR="$cache_dir"
            printf '%s\n' "$version"
            return 0
        fi
    done
    return 1
}

install_local_go() {
    [[ -n "$MISE_BIN" && -x "$MISE_BIN" ]] || {
        die "mise was not found at \$HOME/.local/bin/mise or on PATH"
    }
    mkdir -p -- "$LOCAL_MISE_DATA" "$LOCAL_MISE_CACHE" "$LOCAL_TOOL_ROOT"
    note "Installing Go 1.26.0 through mise into $LOCAL_MISE_DATA"
    MISE_DATA_DIR="$LOCAL_MISE_DATA" MISE_CACHE_DIR="$LOCAL_MISE_CACHE" \
        "$MISE_BIN" install go@1.26.0
}

fetch_pinned_deb() {
    local package_name="$1"
    local package_version="$2"
    local expected_sha="$3"
    local expected_file="$4"
    local target="$SYSROOT_DOWNLOADS/$expected_file"

    if [[ -f "$target" ]]; then
        [[ "$(sha256_file "$target")" == "$expected_sha" ]] ||
            die "downloaded package checksum mismatch: $target"
        return 0
    fi

    command -v apt-get >/dev/null 2>&1 || die "apt-get is required to fetch local sysroot packages"
    mkdir -p -- "$SYSROOT_DOWNLOADS"
    local temporary_dir="$SYSROOT_DOWNLOADS/.apt-$$"
    mkdir -p -- "$temporary_dir"
    (
        cd -- "$temporary_dir"
        DEBIAN_FRONTEND=noninteractive apt-get download "$package_name=$package_version"
    )
    local downloaded="$temporary_dir/$expected_file"
    [[ -f "$downloaded" ]] || die "apt-get download did not produce $expected_file"
    [[ "$(sha256_file "$downloaded")" == "$expected_sha" ]] ||
        die "apt package checksum mismatch: $expected_file"
    mv -- "$downloaded" "$target"
    rmdir -- "$temporary_dir"
}

sysroot_ready() {
    [[ -x "$SYSROOT_BIN/gcc" && -x "$SYSROOT_BIN/g++" ]] &&
        [[ -f "$SYSROOT_INCLUDE/openssl/ssl.h" ]] &&
        [[ -f "$SYSROOT_MULTIARCH_INCLUDE/curl/curl.h" ]] &&
        [[ -e "$SYSROOT_LIB/libssl.so" ]] &&
        [[ -e "$SYSROOT_LIB/libcrypto.so" ]] &&
        [[ -e "$SYSROOT_LIB/libcurl.so" ]]
}

bootstrap_sysroot() {
    command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb is required to extract local sysroot packages"

    mkdir -p -- "$SYSROOT_DOWNLOADS"
    fetch_pinned_deb "libssl-dev" "$SSL_DEV_VERSION" "$SSL_DEV_SHA256" "$SSL_DEV_DEB"
    fetch_pinned_deb "libssl3t64" "$SSL_RUNTIME_VERSION" "$SSL_RUNTIME_SHA256" "$SSL_RUNTIME_DEB"
    fetch_pinned_deb "libcurl4-openssl-dev" "$CURL_DEV_VERSION" "$CURL_DEV_SHA256" "$CURL_DEV_DEB"
    fetch_pinned_deb "libcurl4t64" "$CURL_RUNTIME_VERSION" "$CURL_RUNTIME_SHA256" "$CURL_RUNTIME_DEB"

    if sysroot_ready; then
        note "Local OpenSSL/libcurl sysroot is ready: $SYSROOT_DIR"
        return 0
    fi

    local root_stage="$SYSROOT_DIR/.root-$SYSROOT_ID-$$"
    local bin_stage="$SYSROOT_DIR/.bin-$SYSROOT_ID-$$"
    mkdir -p -- "$root_stage" "$bin_stage"
    dpkg-deb -x "$SYSROOT_DOWNLOADS/$SSL_DEV_DEB" "$root_stage"
    dpkg-deb -x "$SYSROOT_DOWNLOADS/$SSL_RUNTIME_DEB" "$root_stage"
    dpkg-deb -x "$SYSROOT_DOWNLOADS/$CURL_DEV_DEB" "$root_stage"
    dpkg-deb -x "$SYSROOT_DOWNLOADS/$CURL_RUNTIME_DEB" "$root_stage"

    cat >"$bin_stage/gcc" <<EOF
#!/bin/sh
exec /usr/bin/gcc -isystem "$root_stage/usr/include" -isystem "$root_stage/usr/include/x86_64-linux-gnu" -L"$root_stage/usr/lib/x86_64-linux-gnu" "\$@"
EOF
    cat >"$bin_stage/g++" <<EOF
#!/bin/sh
exec /usr/bin/g++ -isystem "$root_stage/usr/include" -isystem "$root_stage/usr/include/x86_64-linux-gnu" -L"$root_stage/usr/lib/x86_64-linux-gnu" "\$@"
EOF
    chmod 700 "$bin_stage/gcc" "$bin_stage/g++"

    [[ ! -e "$SYSROOT_ROOT" ]] ||
        die "incomplete local sysroot exists; inspect and remove it explicitly: $SYSROOT_ROOT"
    [[ ! -e "$SYSROOT_BIN" ]] ||
        die "incomplete local compiler wrappers exist; inspect and remove them explicitly: $SYSROOT_BIN"
    mv -- "$root_stage" "$SYSROOT_ROOT"
    mv -- "$bin_stage" "$SYSROOT_BIN"

    # The wrappers are generated before their final path is known. Rewrite them
    # atomically with the stable sysroot path.
    cat >"$SYSROOT_BIN/.gcc.tmp.$$" <<EOF
#!/bin/sh
exec /usr/bin/gcc -isystem "$SYSROOT_INCLUDE" -isystem "$SYSROOT_MULTIARCH_INCLUDE" -L"$SYSROOT_LIB" "\$@"
EOF
    cat >"$SYSROOT_BIN/.g++.tmp.$$" <<EOF
#!/bin/sh
exec /usr/bin/g++ -isystem "$SYSROOT_INCLUDE" -isystem "$SYSROOT_MULTIARCH_INCLUDE" -L"$SYSROOT_LIB" "\$@"
EOF
    chmod 700 "$SYSROOT_BIN/.gcc.tmp.$$" "$SYSROOT_BIN/.g++.tmp.$$"
    mv -f -- "$SYSROOT_BIN/.gcc.tmp.$$" "$SYSROOT_BIN/gcc"
    mv -f -- "$SYSROOT_BIN/.g++.tmp.$$" "$SYSROOT_BIN/g++"
    sysroot_ready || die "local OpenSSL/libcurl sysroot failed validation"
    note "Extracted pinned Ubuntu packages into local sysroot: $SYSROOT_DIR"
}

if ((INSTALL_GO)); then
    install_local_go
fi

if ((BOOTSTRAP_SYSROOT)); then
    bootstrap_sysroot
fi
if [[ "$MODE" == "build" ]] && ! sysroot_ready; then
    bootstrap_sysroot
fi

source_pin=$(git -C "$SOURCE_REPO" rev-parse HEAD 2>/dev/null || true)
[[ "$source_pin" == "$PINNED_COMMIT" ]] ||
    die "Adam HEAD is '$source_pin', expected $PINNED_COMMIT"
git -C "$SOURCE_REPO" cat-file -e "$PINNED_COMMIT^{commit}" 2>/dev/null ||
    die "pinned commit is not present in the local Adam repository"

required_blobs=(
    "engine/go.mod"
    "drm/Makefile"
    "drm/fairplay-libs.lock"
    "drm/libhybris-core.so"
    "drm/hybris-linker/q.so"
    "scripts/fetch-fairplay-libs.sh"
)
for path in "${required_blobs[@]}"; do
    git -C "$SOURCE_REPO" cat-file -e "$PINNED_COMMIT:$path" 2>/dev/null ||
        die "pinned source is incomplete: missing $path"
done

fetcher_sha=$(git -C "$SOURCE_REPO" show "$PINNED_COMMIT:scripts/fetch-fairplay-libs.sh" | sha256sum | awk '{print $1}')
lock_sha=$(git -C "$SOURCE_REPO" show "$PINNED_COMMIT:drm/fairplay-libs.lock" | sha256sum | awk '{print $1}')
[[ "$fetcher_sha" == "$FAIRPLAY_FETCHER_SHA256" ]] ||
    die "FairPlay fetcher does not match the audited hash"
[[ "$lock_sha" == "$FAIRPLAY_LOCK_SHA256" ]] ||
    die "FairPlay lockfile does not match the audited hash"

PATCH_SHA256="absent"
PATCH_FILES=()
if [[ -d "$PATCH_DIR" ]]; then
    while IFS= read -r pf; do
        [[ -f "$pf" && -s "$pf" ]] || die "patch file is not a non-empty regular file: $pf"
        PATCH_FILES+=("$pf")
    done < <(LC_ALL=C find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' | LC_ALL=C sort)
    if (( ${#PATCH_FILES[@]} > 0 )); then
        PATCH_SHA256=$(
            for pf in "${PATCH_FILES[@]}"; do
                printf '%s %s\n' "$(basename -- "$pf")" "$(sha256_file "$pf")"
            done | sha256sum | awk '{print $1}'
        )
    fi
fi
readonly PATCH_SHA256

BUILD_HELPER_PATH=$(realpath -e -- "${BASH_SOURCE[0]}")
readonly BUILD_HELPER_PATH
BUILD_HELPER_SHA256=$(sha256_lf_normalized_file "$BUILD_HELPER_PATH")
readonly BUILD_HELPER_SHA256
BUILD_RECIPE_SHA256=$(
    printf '%s\n%s\n%s\n' "$PINNED_COMMIT" "$PATCH_SHA256" "$BUILD_HELPER_SHA256" |
        sha256sum | awk '{print $1}'
)
readonly BUILD_RECIPE_SHA256
BUILD_ID=$(printf '%s\n' "$BUILD_RECIPE_SHA256" | sha256sum | awk '{print substr($1,1,16)}')
readonly BUILD_ID
readonly FINAL_BUILD="$LOCAL_BUILD_ROOT/$BUILD_ID"
readonly FINAL_PACKAGE="$STATE_ROOT/builds/$BUILD_ID"

missing=()

check_one_command() {
    local command_name="$1"
    command -v "$command_name" >/dev/null 2>&1 || missing+=("command:$command_name")
}

for command_name in \
    apt-get awk curl dpkg-deb gcc g++ getconf git grep install make pkg-config readlink \
    realpath sed seq sha256sum sort stat tail tar xxd flock setsid
do
    check_one_command "$command_name"
done

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
    missing+=("runtime:Linux x86_64 required")
fi

if sysroot_ready; then
    if ! printf '#include <openssl/ssl.h>\nint main(void){return 0;}\n' |
        "$SYSROOT_BIN/gcc" -x c -fsyntax-only - >/dev/null 2>&1
    then
        missing+=("sysroot:OpenSSL headers failed compiler probe")
    fi
    if ! printf '#include <curl/curl.h>\nint main(void){return 0;}\n' |
        "$SYSROOT_BIN/gcc" -x c -fsyntax-only - >/dev/null 2>&1
    then
        missing+=("sysroot:libcurl headers failed compiler probe")
    fi
else
    missing+=("sysroot:local Ubuntu 24.04 OpenSSL/libcurl packages")
fi

go_version=""
if ! go_version=$(find_mise_go); then
    missing+=("tool:Go 1.26.0 via mise")
fi

note "Glasswave Apple Music Linux helper"
note "  Project:       $PROJECT_ROOT"
note "  Adam source:   $SOURCE_REPO"
note "  Pinned commit: $PINNED_COMMIT"
note "  Patch:         $PATCH_SHA256"
note "  Build id:      $BUILD_ID"
note "  Build tags:    adamplay_backend glasswave"
note "  Runtime state: $STATE_ROOT"
note "  API mode:      GLASSWAVE_HEADLESS=1, http://127.0.0.1:20025"
if [[ -n "$go_version" ]]; then
    note "  Go:            $go_version"
else
    note "  Go:            missing"
fi

if ((${#missing[@]})); then
    note "Missing prerequisites:"
    for item in "${missing[@]}"; do
        case "$item" in
            apt:*)
                note "  ${item#apt:} (Ubuntu package)"
                ;;
            command:*)
                note "  ${item#command:} (command)"
                ;;
            tool:*)
                note "  ${item#tool:}"
                ;;
            sysroot:*)
                note "  ${item#sysroot:}"
                ;;
            runtime:*)
                note "  ${item#runtime:}"
                ;;
            *)
                note "  $item"
                ;;
        esac
    done
    note "No packages were installed."
    note "Prepare local tools without changing system package state:"
    note "  $0 --install-go --bootstrap-sysroot --check --project-root $PROJECT_ROOT"
    exit 1
fi

if [[ "$MODE" == "check" ]]; then
    note "Build preflight passed. No build or download was performed."
    exit 0
fi

mkdir -p -- "$STATE_ROOT" "$STATE_ROOT/builds" "$STATE_ROOT/session" "$LOCAL_BUILD_ROOT"
chmod 700 "$STATE_ROOT" "$STATE_ROOT/builds" "$STATE_ROOT/session" "$LOCAL_BUILD_ROOT"
exec 9>"$STATE_ROOT/build.lock"
flock -n 9 || die "another Glasswave Apple Music build is already running"

FAIRPLAY_DIR="$STATE_ROOT/session/rootfs/system/lib64"
FAIRPLAY_STUBS=(libandroid.so libOpenSLES.so libdl.so)

build_info_value() {
    local build_info="$1"
    local key="$2"
    sed -n "s/^${key}=//p" "$build_info"
}

verify_fairplay_runtime() {
    local lock_file="$STATE_ROOT/session/fairplay-libs.lock"
    [[ -f "$lock_file" ]] || return 1
    [[ "$(sha256_file "$lock_file")" == "$FAIRPLAY_LOCK_SHA256" ]] || return 1

    local expected_sha name target count=0
    while read -r expected_sha name; do
        [[ "$expected_sha" =~ ^[0-9a-f]{64}$ ]] || continue
        [[ "$name" == *.so ]] || continue
        count=$((count + 1))
        target="$FAIRPLAY_DIR/$name"
        [[ -f "$target" ]] || return 1
        [[ "$(sha256_file "$target")" == "$expected_sha" ]] || return 1
    done <"$lock_file"
    ((count > 0)) || return 1

    local stub
    for stub in "${FAIRPLAY_STUBS[@]}"; do
        [[ -f "$FAIRPLAY_DIR/$stub" ]] || return 1
    done
}

install_fairplay_runtime() {
    local fetch_root="$1"
    local fetcher="$fetch_root/scripts/fetch-fairplay-libs.sh"
    local lock_file="$fetch_root/drm/fairplay-libs.lock"
    local stubs_dir="$fetch_root/drm/stubs/out"

    [[ -f "$fetcher" && -f "$lock_file" && -d "$stubs_dir" ]] ||
        die "FairPlay repair source is incomplete under $fetch_root; remove the incomplete cache and rerun Build"
    [[ "$(sha256_file "$fetcher")" == "$FAIRPLAY_FETCHER_SHA256" ]] ||
        die "FairPlay repair fetcher checksum mismatch; remove the incomplete cache and rerun Build"
    [[ "$(sha256_file "$lock_file")" == "$FAIRPLAY_LOCK_SHA256" ]] ||
        die "FairPlay repair lockfile checksum mismatch; remove the incomplete cache and rerun Build"
    local stub
    for stub in "${FAIRPLAY_STUBS[@]}"; do
        [[ -f "$stubs_dir/$stub" ]] ||
            die "FairPlay repair source is missing $stub; remove the incomplete cache and rerun Build"
    done

    mkdir -p -- "$FAIRPLAY_DIR"
    chmod 700 "$STATE_ROOT/session" "$STATE_ROOT/session/rootfs" \
        "$STATE_ROOT/session/rootfs/system" "$FAIRPLAY_DIR"
    note "Fetching or repairing pinned, SHA-256-checked FairPlay runtime libraries"
    "$fetcher" \
        --dest "$FAIRPLAY_DIR" \
        --lock "$lock_file" \
        --stubs "$stubs_dir"
    install -m 0600 "$lock_file" "$STATE_ROOT/session/fairplay-libs.lock"
    verify_fairplay_runtime ||
        die "FairPlay runtime verification failed after repair; remove the incomplete cache and rerun Build"
}

package_files_complete() {
    local package_dir="$1"
    local build_info="$package_dir/build-info.txt"
    [[ -x "$package_dir/engine" ]] &&
        [[ -f "$package_dir/libdrm_client.so" ]] &&
        [[ -f "$package_dir/hybris-linker/q.so" ]] &&
        [[ -f "$build_info" ]] &&
        grep -Fxq "commit=$PINNED_COMMIT" "$build_info" &&
        grep -Fxq "patch_sha256=$PATCH_SHA256" "$build_info" &&
        grep -Fxq "recipe_hash=$BUILD_RECIPE_SHA256" "$build_info" &&
        grep -Fxq "build_id=$BUILD_ID" "$build_info" &&
        grep -Fxq 'build_tags=adamplay_backend glasswave' "$build_info" &&
        grep -Fxq 'headless=1' "$build_info" &&
        [[ "$(build_info_value "$build_info" engine_sha256)" == "$(sha256_file "$package_dir/engine")" ]] &&
        [[ "$(build_info_value "$build_info" libdrm_client_sha256)" == "$(sha256_file "$package_dir/libdrm_client.so")" ]] &&
        [[ "$(sha256_file "$package_dir/hybris-linker/q.so")" == "$Q_SHA256" ]]
}

write_current_build() {
    local temporary="$STATE_ROOT/.current-build.tmp.$$"
    printf '%s\n' "$BUILD_ID" >"$temporary"
    chmod 600 "$temporary"
    mv -f -- "$temporary" "$STATE_ROOT/current-build"
}

if package_files_complete "$FINAL_PACKAGE"; then
    if ! verify_fairplay_runtime; then
        [[ -d "$FINAL_BUILD/src" ]] ||
            die "cached package is missing or has corrupt FairPlay files and its repair source is absent; remove '$FINAL_PACKAGE' and rerun Build"
        install_fairplay_runtime "$FINAL_BUILD/src"
    fi
    verify_fairplay_runtime ||
        die "cache verification failed for q.so, libdrm_client.so, engine, or FairPlay runtime; remove the incomplete cache and rerun Build"
    write_current_build
    note "Build $BUILD_ID is already complete: $FINAL_PACKAGE"
    exit 0
fi

[[ ! -e "$FINAL_PACKAGE" ]] ||
    die "selected build cache is incomplete (engine, libdrm_client.so, q.so, recipe metadata, or hashes); remove '$FINAL_PACKAGE' and '$FINAL_BUILD', then rerun Build"
[[ ! -e "$FINAL_BUILD" ]] ||
    die "selected build cache is incomplete; remove '$FINAL_PACKAGE' and '$FINAL_BUILD', then rerun Build"

STAGE_ROOT="$LOCAL_BUILD_ROOT/.stage-$BUILD_ID-$$"
PACKAGE_STAGE="$STATE_ROOT/builds/.stage-$BUILD_ID-$$"
mkdir -p -- "$STAGE_ROOT/src" "$PACKAGE_STAGE/hybris-linker"

note "Extracting pinned source into isolated build tree"
git -C "$SOURCE_REPO" archive --format=tar "$PINNED_COMMIT" | tar -xf - -C "$STAGE_ROOT/src"

if [[ "$PATCH_SHA256" != "absent" ]]; then
    note "Applying ${#PATCH_FILES[@]} Glasswave patch(es) in isolated source tree"
    # Without its own repository, git apply discovers Glasswave's enclosing .git
    # and treats every upstream path as outside the current prefix (silently
    # skipping the patch). Isolate patch application as well as compilation.
    git -C "$STAGE_ROOT/src" init --quiet
    (
        cd -- "$STAGE_ROOT/src"
        for pf in "${PATCH_FILES[@]}"; do
            git apply --check "$pf"
            git apply "$pf"
        done
    )
else
    note "No Glasswave patches present; building pinned upstream source"
fi

download_lfs_member() {
    local repository_path="$1"
    local expected_sha="$2"
    local expected_size="$3"
    local target="$4"
    local url="https://media.githubusercontent.com/media/silentone12725/apple-music-linux/$PINNED_COMMIT/$repository_path"
    local part="$target.part"

    rm -f -- "$part"
    note "Fetching pinned LFS member: $repository_path"
    curl --proto '=https' --tlsv1.2 -fsSL --retry 3 --connect-timeout 20 --max-time 600 \
        --max-filesize "$expected_size" -o "$part" "$url" ||
        die "download failed: $url"
    [[ "$(stat -c '%s' "$part")" == "$expected_size" ]] ||
        die "size mismatch for $repository_path"
    [[ "$(sha256_file "$part")" == "$expected_sha" ]] ||
        die "SHA-256 mismatch for $repository_path"
    chmod 600 "$part"
    mv -f -- "$part" "$target"
}

download_lfs_member "drm/libhybris-core.so" "$HYBRIS_CORE_SHA256" "$HYBRIS_CORE_SIZE" \
    "$STAGE_ROOT/src/drm/libhybris-core.so"
download_lfs_member "drm/hybris-linker/q.so" "$Q_SHA256" "$Q_SIZE" \
    "$STAGE_ROOT/src/drm/hybris-linker/q.so"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
note "Building libdrm_client.so and Android stubs"
make -C "$STAGE_ROOT/src/drm" -j"$jobs" \
    CC="$SYSROOT_BIN/gcc" CXX="$SYSROOT_BIN/g++" all

note "Building Glasswave headless engine with DRM and without VLC"
(
    cd -- "$STAGE_ROOT/src/engine"
    env \
        CC="$SYSROOT_BIN/gcc" \
        CGO_CFLAGS="-I$SYSROOT_INCLUDE -I$SYSROOT_MULTIARCH_INCLUDE" \
        CGO_ENABLED=1 \
        CGO_LDFLAGS="-L$SYSROOT_LIB -L$STAGE_ROOT/src/drm -Wl,-rpath,\$ORIGIN" \
        GLASSWAVE_HEADLESS=1 \
        MISE_DATA_DIR="$ACTIVE_MISE_DATA_DIR" \
        MISE_CACHE_DIR="$ACTIVE_MISE_CACHE_DIR" \
        "$MISE_BIN" exec go@1.26.0 -- \
        go build -tags 'adamplay_backend glasswave' -trimpath \
        -o "$PACKAGE_STAGE/engine" ./cmd
)

install -m 0755 "$STAGE_ROOT/src/drm/libdrm_client.so" "$PACKAGE_STAGE/libdrm_client.so"
install -m 0644 "$STAGE_ROOT/src/drm/hybris-linker/q.so" "$PACKAGE_STAGE/hybris-linker/q.so"

install_fairplay_runtime "$STAGE_ROOT/src"

cat >"$PACKAGE_STAGE/build-info.txt" <<EOF
commit=$PINNED_COMMIT
patch_sha256=$PATCH_SHA256
build_id=$BUILD_ID
recipe_hash=$BUILD_RECIPE_SHA256
build_helper_sha256=$BUILD_HELPER_SHA256
build_tags=adamplay_backend glasswave
headless=1
engine_sha256=$(sha256_file "$PACKAGE_STAGE/engine")
libdrm_client_sha256=$(sha256_file "$PACKAGE_STAGE/libdrm_client.so")
EOF
chmod 600 "$PACKAGE_STAGE/build-info.txt"

mv -- "$PACKAGE_STAGE" "$FINAL_PACKAGE"
PACKAGE_STAGE=""
mv -- "$STAGE_ROOT" "$FINAL_BUILD"
STAGE_ROOT=""
chmod 700 "$FINAL_PACKAGE" "$FINAL_PACKAGE/hybris-linker"
write_current_build

note "Build complete"
note "  Package: $FINAL_PACKAGE"
note "  Source:  $FINAL_BUILD/src"
note "  State:   $STATE_ROOT/session"
