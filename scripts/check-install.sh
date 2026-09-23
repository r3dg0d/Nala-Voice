#!/usr/bin/env bash
# Verify the install produces everything a package needs, and that what it
# installs actually runs. Registered with ctest as nala-install.
set -euo pipefail
build="${1:?usage: check-install.sh <build-dir>}"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT

DESTDIR="$root" cmake --install "$build" --prefix /usr >/dev/null

expected=(
  usr/bin/nala
  usr/share/applications/nala.desktop
  usr/lib/systemd/user/nala.service
  usr/share/icons/hicolor/scalable/apps/nala.svg
)
status=0
for path in "${expected[@]}"; do
  if [[ -f "$root/$path" ]]; then
    echo "PASS installs $path"
  else
    echo "FAIL missing $path"
    status=1
  fi
done

if [[ -x "$root/usr/bin/nala" ]]; then
  echo "PASS the installed binary is executable"
else
  echo "FAIL the installed binary is not executable"
  status=1
fi

# It has to actually run, not merely exist.
if version="$("$root/usr/bin/nala" --version 2>/dev/null)"; then
  echo "PASS the installed binary runs ($version)"
else
  echo "FAIL the installed binary does not run"
  status=1
fi

# The desktop entry must point at the binary we install.
if grep -qx 'Exec=nala' "$root/usr/share/applications/nala.desktop"; then
  echo "PASS the desktop entry launches nala"
else
  echo "FAIL the desktop entry does not launch nala"
  status=1
fi

# So must the user unit, wherever the prefix put it.
if grep -qx 'ExecStart=/usr/bin/nala' \
    "$root/usr/lib/systemd/user/nala.service" 2>/dev/null; then
  echo "PASS the user unit starts the installed binary"
else
  echo "FAIL the user unit does not start the installed binary"
  status=1
fi

exit "$status"
