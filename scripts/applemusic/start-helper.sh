#!/usr/bin/env bash
# Manage the isolated Glasswave Apple Music helper inside WSL.
#
# The engine listens only on plain HTTP at 127.0.0.1:20025. Authentication is
# always performed by the engine through ADAM_API_TOKEN. Persistent state lives
# on the WSL ext4 filesystem so its 0700 permissions are effective.
set -Eeuo pipefail
umask 077

readonly API_PORT="20025"
readonly API_URL="http://127.0.0.1:${API_PORT}"
readonly GLASSWAVE_HEADLESS="1"
readonly FAIRPLAY_LOCK_SHA256="0f82b05188beac335b2fdc35cee2e625fb050836ecdd0c90f46aa5a78efbf767"
readonly Q_SHA256="b5725b3be908425d028d82b0f549fea5dba396cc3f1ad98bcd5ec9a86ab0b10c"

ACTION=""
PROJECT_ROOT=""
while (($#)); do
    case "$1" in
        start|stop|restart|status|check)
            [[ -z "$ACTION" ]] || {
                printf 'error: more than one action was supplied\n' >&2
                exit 1
            }
            ACTION="$1"
            shift
            ;;
        --project-root)
            [[ $# -ge 2 ]] || {
                printf 'error: --project-root requires a path\n' >&2
                exit 1
            }
            PROJECT_ROOT="$2"
            shift 2
            ;;
        -h|--help)
            ACTION="help"
            shift
            ;;
        *)
            printf 'error: unknown argument: %s\n' "$1" >&2
            exit 1
            ;;
    esac
done

ACTION="${ACTION:-status}"

usage() {
    cat <<'EOF'
Usage: start-helper.sh start|stop|restart|status|check [--project-root PATH]

The API token is read from .local/applemusic/api-token.txt and is never printed.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '%s\n' "$*"
}

if [[ "$ACTION" == "help" ]]; then
    usage
    exit 0
fi

case "$ACTION" in
    start|stop|restart|status|check)
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

if [[ -z "$PROJECT_ROOT" ]]; then
    script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
    PROJECT_ROOT=$(cd -- "$script_dir/../.." && pwd)
fi
PROJECT_ROOT=$(realpath -e -- "$PROJECT_ROOT")

readonly PROJECT_ROOT
readonly TOKEN_FILE="$PROJECT_ROOT/.local/applemusic/api-token.txt"
PROJECT_HASH=$(printf '%s\n' "$PROJECT_ROOT" | sha256sum | awk '{print substr($1,1,16)}')
readonly PROJECT_HASH
STATE_ROOT="${GLASSWAVE_APPLEMUSIC_STATE_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/glasswave-applemusic/$PROJECT_HASH}"
readonly STATE_ROOT
readonly RUNTIME_DIR="$STATE_ROOT/runtime"
readonly SESSION_DIR="$STATE_ROOT/session"
readonly CURRENT_BUILD_FILE="$STATE_ROOT/current-build"
readonly CONFIG_FILE="$RUNTIME_DIR/config.yaml"
readonly PID_FILE="$RUNTIME_DIR/engine.pid"
readonly LOG_FILE="$RUNTIME_DIR/engine.log"

read_token() {
    [[ -f "$TOKEN_FILE" && -r "$TOKEN_FILE" ]] ||
        die "API token file is missing or unreadable: $TOKEN_FILE"
    local token
    token=$(tr -d '\r\n' <"$TOKEN_FILE")
    [[ -n "$token" ]] || die "API token file is empty: $TOKEN_FILE"
    [[ "$token" != *[[:space:]]* ]] || die "API token must not contain whitespace"
    [[ "$token" =~ ^[A-Za-z0-9._~+/=-]+$ ]] || die "API token contains unsupported characters"
    printf '%s' "$token"
}

read_current_build() {
    [[ -f "$CURRENT_BUILD_FILE" ]] ||
        die "no completed build is selected; run setup-applemusic.ps1 -Action Build"
    local build_id
    build_id=$(tr -d '\r\n' <"$CURRENT_BUILD_FILE")
    [[ "$build_id" =~ ^[0-9a-f]{16}$ ]] ||
        die "current-build contains an invalid build id"
    printf '%s' "$build_id"
}

resolve_paths() {
    BUILD_ID=$(read_current_build)
    PACKAGE_DIR="$STATE_ROOT/builds/$BUILD_ID"
    ENGINE="$PACKAGE_DIR/engine"
    DRM_LIBRARY="$PACKAGE_DIR/libdrm_client.so"
    Q_SO="$PACKAGE_DIR/hybris-linker/q.so"
    BUILD_INFO="$PACKAGE_DIR/build-info.txt"
}

verify_build() {
    resolve_paths
    [[ -x "$ENGINE" ]] || die "engine is missing or not executable: $ENGINE"
    [[ -f "$DRM_LIBRARY" ]] || die "libdrm_client.so is missing: $DRM_LIBRARY"
    [[ -f "$Q_SO" ]] || die "hybris linker q.so is missing: $Q_SO"
    [[ "$(sha256sum "$Q_SO" | awk '{print $1}')" == "$Q_SHA256" ]] ||
        die "hybris linker q.so checksum mismatch"
    [[ -f "$BUILD_INFO" ]] || die "build info is missing: $BUILD_INFO"
    grep -Fxq 'build_tags=adamplay_backend glasswave' "$BUILD_INFO" ||
        die "selected build does not use the Glasswave headless build tags"
    grep -Fxq 'headless=1' "$BUILD_INFO" ||
        die "selected build is not marked headless"
    [[ -f "$SESSION_DIR/fairplay-libs.lock" ]] ||
        die "FairPlay library lockfile is missing; rebuild the helper"
    [[ "$(sha256sum "$SESSION_DIR/fairplay-libs.lock" | awk '{print $1}')" == "$FAIRPLAY_LOCK_SHA256" ]] ||
        die "FairPlay library lockfile checksum mismatch"

    local expected_sha name target
    while read -r expected_sha name; do
        [[ "$expected_sha" =~ ^[0-9a-f]{64}$ ]] || continue
        [[ "$name" == *.so ]] || continue
        target="$SESSION_DIR/rootfs/system/lib64/$name"
        [[ -f "$target" ]] || die "FairPlay runtime library is missing: $name"
        [[ "$(sha256sum "$target" | awk '{print $1}')" == "$expected_sha" ]] ||
            die "FairPlay runtime library checksum mismatch: $name"
    done <"$SESSION_DIR/fairplay-libs.lock"
}

write_config() {
    local temporary="$RUNTIME_DIR/.config.yaml.tmp.$$"
    cat >"$temporary" <<EOF
drm-binary-path: "$PACKAGE_DIR"
drm-base-dir: "$SESSION_DIR/files"
alac-max: 192000
get-m3u8-mode: hires
get-m3u8-from-device: true
aac-type: aac-lc
exit-on-error: true
EOF
    chmod 600 "$temporary"
    mv -f -- "$temporary" "$CONFIG_FILE"
}

read_pid() {
    [[ -f "$PID_FILE" ]] || return 1
    local pid
    pid=$(tr -d '\r\n' <"$PID_FILE")
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    printf '%s' "$pid"
}

pid_is_managed_engine() {
    local pid="$1"
    local expected_engine="$2"
    local exe
    local -a arguments=()

    kill -0 "$pid" 2>/dev/null || return 1
    [[ -r "/proc/$pid/exe" ]] || return 1
    exe=$(readlink -f -- "/proc/$pid/exe" 2>/dev/null || true)
    # A newly selected build must still be able to stop the previously selected
    # build. Only executables from this project's private build directory count.
    if [[ "$exe" != "$expected_engine" ]]; then
        local previous_id="${exe#"$STATE_ROOT/builds/"}"
        previous_id="${previous_id%/engine}"
        [[ "$previous_id" =~ ^[0-9a-f]{16}$ ]] || return 1
        [[ "$exe" == "$STATE_ROOT/builds/$previous_id/engine" ]] || return 1
        [[ -f "$STATE_ROOT/builds/$previous_id/build-info.txt" ]] || return 1
        grep -Fxq 'headless=1' "$STATE_ROOT/builds/$previous_id/build-info.txt" || return 1
    fi
    mapfile -d '' arguments <"/proc/$pid/cmdline" || return 1
    [[ "${arguments[0]:-}" == "$exe" ]] || return 1
    [[ " ${arguments[*]} " == *" --api $API_PORT "* ]] || return 1
    return 0
}

port_is_listening() {
    command -v ss >/dev/null 2>&1 || return 1
    ss -ltn "sport = :$API_PORT" 2>/dev/null | tail -n +2 | grep -q .
}

health_check() {
    local token="$1"
    printf 'header = "Authorization: Bearer %s"\n' "$token" |
        curl --proto '=http' --proto-redir '=http' --noproxy '*' -fsS --max-time 5 --config - \
            -o /dev/null "$API_URL/api/v1/status" >/dev/null 2>&1
}

wait_for_health() {
    local token="$1"
    local pid="$2"
    local attempt
    for attempt in $(seq 1 80); do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        if health_check "$token"; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

check_runtime_files() {
    verify_build
    local token
    token=$(read_token)
    [[ -n "$token" ]]
    command -v curl >/dev/null 2>&1 || die "curl is missing"
    command -v readlink >/dev/null 2>&1 || die "readlink is missing"
    note "Build:   $BUILD_ID"
    note "Package: $PACKAGE_DIR"
    note "State:   $STATE_ROOT"
    note "API:     $API_URL"
    note "Token:   present (not displayed)"
}

stop_engine() {
    resolve_paths
    local pid
    if ! pid=$(read_pid); then
        rm -f -- "$PID_FILE"
        note "Apple Music helper is stopped"
        return 0
    fi
    if ! pid_is_managed_engine "$pid" "$ENGINE"; then
        rm -f -- "$PID_FILE"
        note "Apple Music helper is stopped (removed stale pidfile)"
        return 0
    fi

    note "Stopping Apple Music helper (PID $pid)"
    kill -TERM "$pid"
    local attempt
    for attempt in $(seq 1 60); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.25
    done
    if kill -0 "$pid" 2>/dev/null; then
        if pid_is_managed_engine "$pid" "$ENGINE"; then
            kill -KILL "$pid"
        fi
    fi
    rm -f -- "$PID_FILE"
    note "Apple Music helper stopped"
}

start_engine() {
    verify_build
    local token
    token=$(read_token)

    mkdir -p -- "$RUNTIME_DIR" "$RUNTIME_DIR/config" "$RUNTIME_DIR/cache" \
        "$RUNTIME_DIR/data" "$RUNTIME_DIR/tmp" "$RUNTIME_DIR/widevine" \
        "$SESSION_DIR/files/mpl_db"
    chmod 700 "$STATE_ROOT" "$RUNTIME_DIR" "$RUNTIME_DIR/config" \
        "$RUNTIME_DIR/cache" "$RUNTIME_DIR/data" "$RUNTIME_DIR/tmp" \
        "$RUNTIME_DIR/widevine" "$SESSION_DIR" "$SESSION_DIR/files" \
        "$SESSION_DIR/files/mpl_db"

    local existing_pid
    if existing_pid=$(read_pid); then
        if pid_is_managed_engine "$existing_pid" "$ENGINE"; then
            local existing_engine
            existing_engine=$(readlink -f -- "/proc/$existing_pid/exe" 2>/dev/null || true)
            [[ "$existing_engine" == "$ENGINE" ]] ||
                die "selected build differs; use Restart"
            if health_check "$token"; then
                note "Apple Music helper is already running (PID $existing_pid)"
                return 0
            fi
            die "managed helper process exists but its health check failed"
        fi
        rm -f -- "$PID_FILE"
    fi

    if port_is_listening; then
        die "127.0.0.1:$API_PORT is already in use by an unmanaged process"
    fi

    write_config
    # A Windows client forwarded by WSL has no matching client socket in Linux's
    # /proc table. The fixed loopback listener and mandatory random API token are
    # the authentication boundary for this embedding instead of Linux peer UID.
    (
        cd -- "$RUNTIME_DIR"
        env \
            ADAM_API_TOKEN="$token" \
            ADAM_ALLOW_OTHER_USERS=1 \
            ADAM_ADAMPLAY_DIR="$RUNTIME_DIR/widevine" \
            XDG_CONFIG_HOME="$RUNTIME_DIR/config" \
            XDG_CACHE_HOME="$RUNTIME_DIR/cache" \
            XDG_DATA_HOME="$RUNTIME_DIR/data" \
            TMPDIR="$RUNTIME_DIR/tmp" \
            GLASSWAVE_HEADLESS="$GLASSWAVE_HEADLESS" \
            nohup setsid "$ENGINE" --api "$API_PORT" \
            </dev/null >>"$LOG_FILE" 2>&1 9>&- &
        printf '%s\n' "$!" >"$RUNTIME_DIR/.engine.pid.tmp"
    )
    chmod 600 "$RUNTIME_DIR/.engine.pid.tmp" "$LOG_FILE"
    mv -f -- "$RUNTIME_DIR/.engine.pid.tmp" "$PID_FILE"

    local pid
    pid=$(read_pid)
    if ! wait_for_health "$token" "$pid"; then
        stop_engine >/dev/null
        die "helper did not become healthy; inspect $LOG_FILE"
    fi
    note "Apple Music helper started (PID $pid)"
    note "API: $API_URL"
}

show_status() {
    verify_build
    local token
    token=$(read_token)
    local pid
    if ! pid=$(read_pid) || ! pid_is_managed_engine "$pid" "$ENGINE"; then
        note "Apple Music helper is stopped"
        return 1
    fi
    if health_check "$token"; then
        note "Apple Music helper is running (PID $pid)"
        note "API: $API_URL"
        return 0
    fi
    note "Apple Music helper process $pid exists but is unhealthy"
    return 1
}

case "$ACTION" in
    check)
        check_runtime_files
        ;;
    start)
        start_engine
        ;;
    stop)
        stop_engine
        ;;
    restart)
        stop_engine
        start_engine
        ;;
    status)
        show_status
        ;;
esac
