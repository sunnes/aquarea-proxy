#!/usr/bin/env bash
# Command line tester for the Aquarea proxy HTTP API.
# Usage: scripts/aquarea.sh [-y] [-n] <command> [args]      (see "help")
set -uo pipefail

# Settings: environment variables win, else secrets.env in the repo root (gitignored,
# see secrets.env.example). There are no built-in defaults.
SECRETS_FILE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/secrets.env"
secret() {   # secret KEY -> value from secrets.env (nothing if missing); the file is parsed, not sourced
  [ -r "$SECRETS_FILE" ] || return 0
  sed -n "s/^$1=//p" "$SECRETS_FILE" | head -1 | tr -d "\"'"
}
HOST="${AQUAREA_HOST:-$(secret AQUAREA_HOST)}"
API_USER="${AQUAREA_USER:-aquarea}"
API_PASS="${AQUAREA_PASS:-$(secret DEVICE_PASSWORD)}"
YES=0                                      # -y : do not ask before changing anything
DRY=0                                      # -n : only print the curl commands

usage() {
  cat <<USAGE
Aquarea proxy tester  (host: ${HOST:-not set})

  status                compact summary: power, mode, temperatures, power draw, errors
  state                 power, control status and all decoded registers; the last column
                        shows the key to use with 'set' for changeable settings
  metrics [text]        the Prometheus metrics (optionally only lines containing <text>)
  read <reg>            read one register from the heat pump (harmless)
  on | off              heating on / off          (changes the heat pump!)
  mode <name>           operating mode, keeps on/off. Names: heat cool auto tank
                        heat+tank cool+tank auto+tank   ('auto' is inferred, see notes)
  settings [-r]         settable temperatures: value, limits, age; -r reads them from the
                        heat pump first (slow, ~1-4 s each) instead of showing the last seen
  set <key> <value>     change a setting (see 'state'/'settings' for keys), e.g. set tank_set 50 (changes the heat pump!)
  reset-error           clear the error           (changes the heat pump!)
  test                  read-only checks, safe to run any time
  test-power            guided cycle: ON -> verify -> OFF -> verify (asks first)
  test-mode [name]      guided: switch to <name> (default: heat<->tank), verify, switch back
  test-modes            guided: walk through ALL modes and restore the start mode (unit must be OFF)
  help

Options:  -y  skip confirmations     -n  dry run (print the curl commands only)
Config:   AQUAREA_HOST  AQUAREA_PASS  (env, or AQUAREA_HOST / DEVICE_PASSWORD in secrets.env)
          AQUAREA_USER (default aquarea)

Examples:
  scripts/aquarea.sh test
  scripts/aquarea.sh read 144
  scripts/aquarea.sh set flow_low 38
  scripts/aquarea.sh on
  scripts/aquarea.sh mode heat+tank
  AQUAREA_HOST=aquarea.local scripts/aquarea.sh state
USAGE
}

have_jq() { command -v jq >/dev/null 2>&1; }

# api METHOD PATH  -> prints the body, sets API_CODE and API_BODY, returns 0 on HTTP 2xx
API_CODE=""; API_BODY=""
api() {
  local method="$1" path="$2" url="http://$HOST$2"
  if [ "$DRY" = 1 ]; then
    echo "curl -sS --max-time 10 -u $API_USER:*** -X $method '$url'"
    API_CODE=200; API_BODY='{}'
    return 0
  fi
  local resp
  if ! resp=$(curl -sS --max-time "${API_TIMEOUT:-10}" -u "$API_USER:$API_PASS" -X "$method" -w $'\n%{http_code}' "$url" 2>&1); then
    echo "request failed: $resp" >&2
    API_CODE=000; API_BODY=""
    return 1
  fi
  API_CODE="${resp##*$'\n'}"
  API_BODY="${resp%$'\n'*}"
  case "$API_CODE" in
    2??) return 0 ;;
    401) echo "HTTP 401: wrong user/password (AQUAREA_USER / AQUAREA_PASS)" >&2; return 1 ;;
    *)   return 1 ;;
  esac
}

pretty() { if have_jq; then jq -c . <<<"$1" 2>/dev/null || echo "$1"; else echo "$1"; fi; }

confirm() {
  [ "$YES" = 1 ] && return 0
  [ "$DRY" = 1 ] && return 0
  local ans
  read -r -p "$1 [y/N] " ans
  [ "$ans" = "y" ] || [ "$ans" = "Y" ]
}

power_state() {   # prints on / off / unknown
  api GET /api/state >/dev/null 2>&1 || { echo unknown; return; }
  local p
  p=$(sed -n 's/.*"power": *"\([a-z]*\)".*/\1/p' <<<"$API_BODY")
  echo "${p:-unknown}"
}

cmd_state() {
  api GET /api/state || { echo "$API_BODY"; return 1; }
  [ "$DRY" = 1 ] && return 0
  if have_jq; then
    jq -r '"uptime \(.uptime_s)s  power \(.power // "?")  forwarding \(.forwarding)  control \(.control.state)",
           "txn \(.stats.txn)  sets \(.stats.sets)  nak \(.stats.naks)  noreply \(.stats.noreply)  unmatched \(.stats.unmatched)"' <<<"$API_BODY"
    echo
    jq -r '.registers | to_entries | sort_by(.key|tonumber)[] |
           "#\(.key)\t\(.value.name)\t\(.value.text)\t(raw \(.value.raw), \(.value.age_s)s ago)\t\(if .value.set_key then "set key: \(.value.set_key)" else "" end)"' <<<"$API_BODY" | column -t -s $'\t'
  else
    echo "$API_BODY"
  fi
}

cmd_metrics() {
  local url="http://$HOST/metrics" out
  if [ "$DRY" = 1 ]; then echo "curl -sS --max-time 10 -u $API_USER:*** '$url'"; return 0; fi
  if ! out=$(curl -sS --max-time 10 -u "$API_USER:$API_PASS" -f "$url" 2>&1); then
    echo "request failed: $out" >&2; return 1
  fi
  if [ -n "${1:-}" ]; then grep -- "$1" <<<"$out" | grep -v '^#' || true; else echo "$out"; fi
}

cmd_read() {
  local reg="${1:-}"
  [[ "$reg" =~ ^[0-9]+$ ]] && [ "$reg" -le 254 ] || { echo "usage: read <reg 0..254>" >&2; return 2; }
  api GET "/api/read?reg=$reg"; local rc=$?
  pretty "$API_BODY"; return $rc
}

cmd_change() {   # cmd_change "<question>" METHOD PATH
  confirm "$1" || { echo "cancelled"; return 1; }
  api POST "$3"; local rc=$?
  pretty "$API_BODY"
  [ $rc -eq 0 ] || echo "FAILED (HTTP $API_CODE)" >&2
  return $rc
}

stat_of() { sed -n 's/.*"'"$1"'": *\([0-9][0-9]*\).*/\1/p' <<<"$API_BODY"; }

PASS_N=0; FAIL_N=0
check() {   # check "<name>" <exit code>
  if [ "$2" -eq 0 ]; then echo "  PASS  $1"; PASS_N=$((PASS_N+1)); else echo "  FAIL  $1"; FAIL_N=$((FAIL_N+1)); fi
}

cmd_test() {
  echo "Read-only checks against $HOST"
  api GET /api/state >/dev/null; check "GET /api/state answers (HTTP $API_CODE)" $?
  local n; n=$(grep -o '"name"' <<<"$API_BODY" | wc -l | tr -d ' ')
  [ "${n:-0}" -ge 10 ]; check "register table has entries ($n known)" $?
  grep -Eq '"forwarding": ?true' <<<"$API_BODY"; check "forwarding is on" $?
  local b_txn b_nr b_nak b_um a_txn a_nr a_nak a_um
  b_txn=$(stat_of txn); b_nr=$(stat_of noreply); b_nak=$(stat_of naks); b_um=$(stat_of unmatched)
  echo "        watching the bus for 5 s ..."
  sleep 5
  api GET /api/state >/dev/null
  a_txn=$(stat_of txn); a_nr=$(stat_of noreply); a_nak=$(stat_of naks); a_um=$(stat_of unmatched)
  [ "${a_txn:-0}" -gt "${b_txn:-0}" ]; check "bus is alive: transactions ${b_txn:-?} -> ${a_txn:-?} in 5 s" $?
  [ "${a_nr:-1}" = "${b_nr:-0}" ] && [ "${a_nak:-1}" = "${b_nak:-0}" ] && [ "${a_um:-1}" = "${b_um:-0}" ]
  check "no unanswered / NAK / unmatched in 5 s (lifetime: noreply=${a_nr:-?} naks=${a_nak:-?} unmatched=${a_um:-?})" $?
  local reg
  for reg in 27 144 18; do
    api GET "/api/read?reg=$reg" >/dev/null; local rc=$?
    echo "        read #$reg -> $(tr -d '\n' <<<"$API_BODY")"
    check "read register $reg through the proxy" $rc
  done
  echo "Result: $PASS_N passed, $FAIL_N failed"
  [ "$FAIL_N" -eq 0 ]
}

wait_power() {   # wait_power on|off  (up to 15 s)
  local want="$1" i s
  for i in $(seq 1 15); do
    s=$(power_state)
    echo "        t+${i}s power: $s"
    [ "$s" = "$want" ] && return 0
    sleep 1
  done
  return 1
}

cmd_test_power() {
  local start; start=$(power_state)
  echo "Current power state: $start"
  [ "$start" = "unknown" ] && { echo "cannot read the power state, aborting"; return 1; }
  confirm "Cycle heating ON, verify, then OFF (starting from $start)?" || { echo "cancelled"; return 1; }
  if [ "$start" = "off" ]; then
    api POST "/api/power?on=1"; check "power ON accepted: $(tr -d '\n' <<<"$API_BODY")" $?
    wait_power on; check "status flag shows ON" $?
    confirm "Turn heating OFF again?" && { api POST "/api/power?on=0"; check "power OFF accepted: $(tr -d '\n' <<<"$API_BODY")" $?; wait_power off; check "status flag shows OFF" $?; }
  else
    api POST "/api/power?on=0"; check "power OFF accepted: $(tr -d '\n' <<<"$API_BODY")" $?
    wait_power off; check "status flag shows OFF" $?
    confirm "Turn heating ON again?" && { api POST "/api/power?on=1"; check "power ON accepted: $(tr -d '\n' <<<"$API_BODY")" $?; wait_power on; check "status flag shows ON" $?; }
  fi
  echo "Result: $PASS_N passed, $FAIL_N failed"
  [ "$FAIL_N" -eq 0 ]
}

MODES="heat cool auto tank heat+tank cool+tank auto+tank"

norm_mode() { tr '_ ' '++' <<<"$1"; }                     # heat_tank / "heat tank" -> heat+tank
valid_mode() { local m; for m in $MODES; do [ "$m" = "$1" ] && return 0; done; return 1; }

mode_state() {   # last commanded/known mode: heat / tank / heat+tank / ... / unknown
  api GET /api/state >/dev/null 2>&1 || { echo unknown; return; }
  local m
  m=$(sed -n 's/.*"mode": *"\([a-z+]*\)".*/\1/p' <<<"$API_BODY")
  echo "${m:-unknown}"
}

# Register 27 (status flags) shows what the heat pump really does: bit 2 heat, 4 cool, 16 tank.
# 'auto' has no flag bit, so for auto modes only "heat and cool bits are clear" can be verified.
expected_flags() {   # expected_flags <mode> -> expected value of (flags & 22)
  local e=0
  case "$1" in *heat*) e=$((e | 2)) ;; esac
  case "$1" in *cool*) e=$((e | 4)) ;; esac
  case "$1" in *tank*) e=$((e | 16)) ;; esac
  echo "$e"
}

wait_mode() {   # wait_mode <mode>  (up to 15 s, polls the status flags)
  local want i f
  want=$(expected_flags "$1")
  for i in $(seq 1 15); do
    api GET /api/state >/dev/null 2>&1
    f=$(sed -n 's/.*"flags": *\([0-9][0-9]*\).*/\1/p' <<<"$API_BODY")
    echo "        t+${i}s command: $(sed -n 's/.*"mode": *"\([a-z+]*\)".*/\1/p' <<<"$API_BODY")  flags: ${f:-?}"
    if [ -n "$f" ] && [ $((f & 22)) -eq "$want" ]; then return 0; fi
    sleep 1
  done
  return 1
}

cmd_status() {
  api GET /api/state || { echo "$API_BODY"; return 1; }
  [ "$DRY" = 1 ] && return 0
  if ! have_jq; then echo "$API_BODY"; return 0; fi
  jq -r '
    def r(k): (.registers[k].text // "?");
    "power:       \((.power // "?") | ascii_upcase)   mode: \(.mode // "?")   (status flags raw \(.flags // "?"))   forwarding: \(.forwarding)",
    "temperature: outdoor \(r("18"))  |  flow \(r("19"))  |  return \(r("21"))  |  tank \(r("22"))",
    "heat pump:   compressor \(r("23"))  |  pump \(r("35"))  |  defrost \(r("17"))",
    "power draw:  heating \(r("29"))  |  tank \(r("33"))  |  cooling \(r("31"))",
    "errors:      current \(r("20"))  |  last \(r("24"))",
    "bus:         \(.stats.txn) transactions, \(.stats.noreply) unanswered, \(.stats.naks) NAK, uptime \(.uptime_s)s"
  ' <<<"$API_BODY"
}

cmd_mode() {
  local m
  m=$(norm_mode "${1:-}")
  if ! valid_mode "$m"; then echo "usage: mode <$(sed 's/ /|/g' <<<"$MODES")>" >&2; return 2; fi
  cmd_change "Switch operating mode to $m (current: $(mode_state))?" POST "/api/mode?m=${m//+/%2B}"
}

cmd_settings() {
  local path=/api/settings
  if [ "${1:-}" = "-r" ]; then path="/api/settings?refresh=1"; API_TIMEOUT=60; fi
  api GET "$path" || { echo "$API_BODY"; return 1; }
  [ "$DRY" = 1 ] && return 0
  if have_jq; then
    jq -r '.settings | to_entries[] |
      "\(.key)\t#\(.value.reg)\t\(.value.text)\t(\(.value.min)..\(.value.max))\t\(if .value.age_s then "\(.value.age_s)s ago" else "never read" end)\(if .value.error then "  ERROR: \(.value.error)" else "" end)"' \
      <<<"$API_BODY" | column -t -s $'\t'
  else echo "$API_BODY"; fi
}

cmd_set() {
  local k="${1:-}" v="${2:-}"
  [[ "$k" =~ ^[a-z_]+$ ]] && [[ "$v" =~ ^-?[0-9]+$ ]] || { echo "usage: set <key> <integer C>   (keys: see 'settings')" >&2; return 2; }
  cmd_change "Set $k = $v?" POST "/api/set?k=$k&v=$v"
}

cmd_test_mode() {   # test-mode [target]
  local start target
  start=$(mode_state)
  echo "Current mode: $start   power: $(power_state)"
  if [ -n "${1:-}" ]; then
    target=$(norm_mode "$1")
    valid_mode "$target" || { echo "unknown mode: $1" >&2; return 2; }
  else
    case "$start" in
      heat) target=tank ;;
      tank) target=heat ;;
      *) echo "current mode is '$start': pass a target mode, e.g. test-mode heat" >&2; return 1 ;;
    esac
  fi
  confirm "Switch mode $start -> $target, verify, then switch back?" || { echo "cancelled"; return 1; }
  api POST "/api/mode?m=${target//+/%2B}"; check "mode $target accepted: $(tr -d '\n' <<<"$API_BODY")" $?
  wait_mode "$target"; check "status flags match $target" $?
  if [ "$start" != "unknown" ] && confirm "Switch back to $start?"; then
    api POST "/api/mode?m=${start//+/%2B}"; check "mode $start accepted: $(tr -d '\n' <<<"$API_BODY")" $?
    wait_mode "$start"; check "status flags match $start" $?
  fi
  echo "Result: $PASS_N passed, $FAIL_N failed"
  [ "$FAIL_N" -eq 0 ]
}

cmd_test_modes() {
  local start p m
  p=$(power_state)
  if [ "$p" != "off" ]; then echo "heating must be OFF for test-modes (power: $p). Run: $0 off" >&2; return 1; fi
  start=$(mode_state)
  echo "Start mode: $start (power off). Will visit: $MODES"
  confirm "Walk through all modes and restore '$start'?" || { echo "cancelled"; return 1; }
  for m in $MODES; do
    api POST "/api/mode?m=${m//+/%2B}"; check "mode $m accepted" $?
    wait_mode "$m"; check "status flags match $m" $?
  done
  if valid_mode "$start"; then
    api POST "/api/mode?m=${start//+/%2B}"; check "restored $start" $?
    wait_mode "$start"; check "status flags match $start" $?
  else
    echo "start mode '$start' is not restorable by name; leaving the last tested mode set"
  fi
  echo "Result: $PASS_N passed, $FAIL_N failed"
  [ "$FAIL_N" -eq 0 ]
}

while getopts "yn" opt; do
  case "$opt" in y) YES=1 ;; n) DRY=1 ;; *) usage; exit 2 ;; esac
done
shift $((OPTIND - 1))

case "${1:-help}" in
  help|-h|--help) ;;
  *) if [ -z "$HOST" ] || { [ -z "$API_PASS" ] && [ "$DRY" != 1 ]; }; then
       echo "AQUAREA_HOST / password not set: create secrets.env (see secrets.env.example) or export AQUAREA_HOST and AQUAREA_PASS" >&2
       exit 2
     fi ;;
esac

case "${1:-help}" in
  status)       cmd_status ;;
  state)        cmd_state ;;
  metrics)      shift; cmd_metrics "${1:-}" ;;
  read)         shift; cmd_read "${1:-}" ;;
  on)           cmd_change "Turn heating ON?" POST "/api/power?on=1" ;;
  off)          cmd_change "Turn heating OFF?" POST "/api/power?on=0" ;;
  mode)         shift; cmd_mode "${1:-}" ;;
  settings)     shift; cmd_settings "${1:-}" ;;
  set)          shift; cmd_set "${1:-}" "${2:-}" ;;
  reset-error)  cmd_change "Reset the heat pump error?" POST "/api/reset_error" ;;
  test)         cmd_test ;;
  test-power)   cmd_test_power ;;
  test-mode)    shift; cmd_test_mode "${1:-}" ;;
  test-modes)   cmd_test_modes ;;
  help|-h|--help) usage ;;
  *)            echo "unknown command: $1" >&2; usage; exit 2 ;;
esac
