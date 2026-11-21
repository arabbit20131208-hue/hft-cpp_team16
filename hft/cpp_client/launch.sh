#!/usr/bin/env bash
# launch.sh — build (if needed) and start the Phase 4 bot.
#
#   ./hft/cpp_client/launch.sh                 gateway + strategy (the gateway starts and
#                                              supervises the strategy process)
#   ./hft/cpp_client/launch.sh --replay < tape offline replay through both processes
#   ./hft/cpp_client/launch.sh --no-spawn      gateway only; run the strategy yourself:
#   ./hft/cpp_client/launch.sh strategy        ...in a second terminal (kill it, start it
#                                              again: the gateway keeps its connection)
#
# Venue settings come from the environment or the repo-root .env
# (TEAM_ID, EXCHANGE_URL, ARENA_TOKEN, ARENA_LEVEL). BUILD selects the build dir.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build_phase4}"

if [[ ! -x "$BUILD/hft_bot" || ! -x "$BUILD/strategy" ]]; then
  echo "[launch] building into $BUILD" >&2
  cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DHFT_USE_TLS=ON >&2
  cmake --build "$BUILD" -j >&2
fi

if [[ -f "$ROOT/.env" ]]; then
  set -a
  # shellcheck disable=SC1091
  source "$ROOT/.env"
  set +a
fi

if [[ "${1:-}" == "strategy" ]]; then
  export HFT_SHM="${HFT_SHM:-/hft_team16}"
  exec "$BUILD/strategy"
fi
for arg in "$@"; do
  # A strategy started by hand must find the region by a fixed name.
  [[ "$arg" == "--no-spawn" ]] && export HFT_SHM="${HFT_SHM:-/hft_team16}"
done
exec "$BUILD/hft_bot" "$@"
