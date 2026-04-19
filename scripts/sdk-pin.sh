#!/usr/bin/env bash
# Pin ufbt to the exact Unleashed SDK version this project is known to build
# against. Running plain `ufbt update --channel=release` tracks the moving
# release head, so a future release could introduce a breaking API change
# and silently fail our builds. This script locks to a specific zip.
#
# To bump the pin:
#   1. Verify the new SDK builds the FAP cleanly (run on-device validation)
#   2. Update PINNED_VERSION, PINNED_URL, and PINNED_SHA256 below
#   3. Commit the change with the new SDK version in the message
#   4. Also bump the CI workflow pin in .github/workflows/ if it references
#      the URL directly

set -euo pipefail

PINNED_VERSION="unlshd-086"
PINNED_URL="https://unleashedflip.com/fw/unlshd-086/flipper-z-f7-sdk-unlshd-086.zip"
PINNED_SHA256="ffb4a07976073d079a5c56717a785e27a4167dff4cf7b755db66b24ff1e3e588"

echo ">>> Pinning ufbt SDK to ${PINNED_VERSION}"
echo ">>> URL: ${PINNED_URL}"
echo ">>> SHA256: ${PINNED_SHA256}"

# Optional pre-flight checksum verify. Skipped if curl/shasum unavailable.
if command -v curl >/dev/null 2>&1 && command -v shasum >/dev/null 2>&1; then
    TMP=$(mktemp -t sdk-pin.XXXXXX.zip)
    trap 'rm -f "$TMP"' EXIT
    echo ">>> Downloading + verifying checksum..."
    curl -fL "${PINNED_URL}" -o "${TMP}"
    ACTUAL=$(shasum -a 256 "${TMP}" | cut -d' ' -f1)
    if [ "${ACTUAL}" != "${PINNED_SHA256}" ]; then
        echo "!!! SHA256 mismatch — expected ${PINNED_SHA256}, got ${ACTUAL}"
        echo "!!! The pinned URL has likely been re-published. Investigate before continuing."
        exit 1
    fi
    echo ">>> Checksum OK"
    ufbt update --hw-target=f7 --local="${TMP}"
else
    # Fallback: skip verify, let ufbt download
    ufbt update --hw-target=f7 --url="${PINNED_URL}"
fi

echo ">>> Done. Build with: cd claude_buddy && ufbt"
