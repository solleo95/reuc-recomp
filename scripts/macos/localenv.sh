# Sourced by the macOS scripts: loads local.env (KEY=VALUE lines, "#" comments) from the project root; a variable already set in the shell wins
# (as scripts/local.bat and scripts/localenv.py). Expects ROOT.
if [[ -f "$ROOT/local.env" ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    [[ -z "$line" || "$line" == \#* || "$line" != *=* ]] && continue
    key="${line%%=*}"; value="${line#*=}"
    key="${key//[[:space:]]/}"
    [[ -n "$key" && -z "${!key+set}" ]] && export "$key=$value"
  done < "$ROOT/local.env"
fi
