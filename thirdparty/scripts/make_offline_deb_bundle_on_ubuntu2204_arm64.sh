#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/debs/offline_bundle}"

if [[ "$(dpkg --print-architecture)" != "arm64" ]]; then
  echo "error: run this script on an Ubuntu 22.04 arm64 system" >&2
  echo "for example, run it on an online RK3588/MGX10V device or arm64 VM" >&2
  exit 1
fi

. /etc/os-release
if [[ "${ID}" != "ubuntu" || "${VERSION_ID}" != "22.04" ]]; then
  echo "error: expected Ubuntu 22.04 arm64, got ${PRETTY_NAME}" >&2
  exit 1
fi

PACKAGES=(
  libzmq5
  libzmq3-dev
  cppzmq-dev
  libprotobuf23
  libprotobuf-dev
  libprotoc23
  protobuf-compiler
  pkg-config
  zlib1g-dev
)

mkdir -p "${OUT_DIR}"

echo "Updating apt metadata..."
sudo apt-get update

echo "Resolving dependency closure..."
apt-cache depends --recurse \
  --no-recommends \
  --no-suggests \
  --no-conflicts \
  --no-breaks \
  --no-replaces \
  --no-enhances \
  "${PACKAGES[@]}" \
  | awk '/^[[:alnum:]][[:alnum:].+:-]*$/ {print $1}' \
  | sort -u > "${OUT_DIR}/package-list.txt"

echo "Downloading packages to ${OUT_DIR}..."
(
  cd "${OUT_DIR}"
  xargs -r apt-get download < package-list.txt
)

cat > "${OUT_DIR}/install_on_device.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "error: run as root on the device" >&2
  exit 1
fi

if ! compgen -G "${SCRIPT_DIR}/*.deb" >/dev/null; then
  echo "error: no .deb files found in ${SCRIPT_DIR}" >&2
  exit 1
fi

dpkg -i "${SCRIPT_DIR}"/*.deb || true
apt-get --no-download -f install -y

echo
echo "Installed packages:"
dpkg -l | awk '/protobuf|libzmq|cppzmq|zeromq/ {print $1, $2, $3}'
EOF

chmod +x "${OUT_DIR}/install_on_device.sh"

echo
echo "Offline bundle is ready:"
echo "  ${OUT_DIR}"
echo
echo "Copy this directory to the device and run:"
echo "  cd <copied offline_bundle>"
echo "  ./install_on_device.sh"

