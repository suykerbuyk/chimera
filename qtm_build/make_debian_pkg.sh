#!/bin/bash -e

VERSION=$1
REVISION=$2
ARCH=$3
# Git commit id the packages were built from, stamped into the control
# files as Quantum-Git-Commit (query with dpkg -s).  qb exports it into
# the build container as QB_BUILD_GIT_SHA (with a -dirty suffix when the
# source tree has local changes).
COMMIT=${QB_BUILD_GIT_SHA:-unknown}

PKG_DIR="$(basename $PWD)"

INSTALL_ROOT="$PWD"
LIBDIR="${INSTALL_ROOT}/opt/quantum/chimera/lib"

# Split the validation test suites into their own package (the Debian
# equivalent of the quantum-chimera-tests RPM subpackage): move
# opt/quantum/chimera/tests out of the server tree before it is packed.
TESTS_PKG_DIR="quantum-chimera-tests_${VERSION}-${REVISION}_${ARCH}"
TESTS_ROOT="$(dirname "${INSTALL_ROOT}")/${TESTS_PKG_DIR}"
if [ -d "${INSTALL_ROOT}/opt/quantum/chimera/tests" ]; then
    mkdir -p "${TESTS_ROOT}/opt/quantum/chimera"
    mv "${INSTALL_ROOT}/opt/quantum/chimera/tests" \
       "${TESTS_ROOT}/opt/quantum/chimera/tests"
fi

mkdir -p ./DEBIAN
cp -r /context/qtm_build/debian/* ./DEBIAN/
# *.tmpl are only input templates; do not ship them inside the package.
rm -f ./DEBIAN/control.tmpl ./DEBIAN/control-tests.tmpl

# Compute the shared-library dependencies automatically. This is the Debian
# equivalent of RPM's automatic dependency generation: dpkg-deb --build does
# not run dpkg-shlibdeps, so we invoke it directly against the installed ELF
# objects. Generating the list per build keeps it correct across Ubuntu
# releases, including the 64-bit time_t package renames (e.g. libssl3 ->
# libssl3t64, liburcu8 -> liburcu8t64), which a hardcoded list cannot track.
ELF_MAGIC=$(printf '\177ELF')
OBJS=()
while IFS= read -r f; do
    [ "$(dd if="$f" bs=1 count=4 2>/dev/null)" = "$ELF_MAGIC" ] && OBJS+=("$f")
done < <(find "${INSTALL_ROOT}/opt/quantum/chimera/lib" \
              "${INSTALL_ROOT}/opt/quantum/chimera/bin" \
              "${INSTALL_ROOT}/opt/quantum/chimera/sbin" -type f 2>/dev/null)

if [ "${#OBJS[@]}" -eq 0 ]; then
    echo "ERROR: no ELF objects found under ${INSTALL_ROOT}/opt/quantum/chimera" >&2
    exit 1
fi

# dpkg-shlibdeps reads debian/control (lowercase); provide a minimal one in a
# scratch directory so it does not collide with the DEBIAN/ control area.
WORK="$(mktemp -d)"
mkdir -p "${WORK}/debian"
cat > "${WORK}/debian/control" <<EOF
Source: quantum-chimera-server

Package: quantum-chimera-server
Architecture: ${ARCH}
EOF
# -l adds the bundled lib dir so the chimera/evpl private libs resolve, and
# --ignore-missing-info skips them (they have no dpkg shlibs info) instead of
# failing, leaving only system-package dependencies. Capture the raw output
# first (not through a pipe) so a dpkg-shlibdeps failure aborts under set -e
# rather than being masked by the following sed.
RAW_DEPS="$(cd "${WORK}" && \
    dpkg-shlibdeps -O --ignore-missing-info -l"${LIBDIR}" "${OBJS[@]}")"
rm -rf "${WORK}"
SHLIB_DEPS="$(printf '%s\n' "${RAW_DEPS}" | sed -n 's/^shlibs:Depends=//p')"

if [ -z "${SHLIB_DEPS}" ]; then
    echo "ERROR: dpkg-shlibdeps produced no shared-library dependencies" >&2
    exit 1
fi

# Non-ELF runtime dependencies that dpkg-shlibdeps cannot detect.
EXTRA_DEPS="python3, python3-requests"
DEPENDS="${SHLIB_DEPS:+${SHLIB_DEPS}, }${EXTRA_DEPS}"

sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__REVISION__/${REVISION}/" \
    -e "s/__ARCH__/${ARCH}/" \
    -e "s/__COMMIT__/${COMMIT}/" \
    -e "s|__DEPENDS__|${DEPENDS}|" \
    /context/qtm_build/debian/control.tmpl > ./DEBIAN/control
mkdir -p etc/ld.so.conf.d/
echo "/opt/quantum/chimera/lib" > etc/ld.so.conf.d/chimera.conf

# Version stamp for quick reference on installed systems; the same
# information is carried in the package metadata (Quantum-Git-Commit).
# Deliberately not a conffile: it must be replaced on every upgrade.
mkdir -p opt/quantum/chimera/etc
cat > opt/quantum/chimera/etc/version <<EOF
version: ${VERSION}-${REVISION}
commit: ${COMMIT}
EOF

# Runtime state directory (CHIMERA_STATE_DIR): holds secrets persisted by the
# server (NFS file-handle signing key, SMB server GUID), so keep it 0700.
# dpkg-deb preserves empty directories, so shipping it here is enough for
# fresh installs; postinst re-creates it for upgrades from older packages.
mkdir -p opt/quantum/chimera/share/state
chmod 700 opt/quantum/chimera/share/state

cd ..
dpkg-deb --build ${PKG_DIR}
mv ${PKG_DIR}.deb /dist/

# Build the quantum-chimera-tests package from the tree split off above.
# Its binaries link the bundled chimera/evpl libs shipped by the server
# package, so the shlibdeps scan resolves them against the server tree's
# lib dir and the package depends on the exact server version.
if [ -d "${TESTS_ROOT}" ]; then
    TEST_OBJS=()
    while IFS= read -r f; do
        [ "$(dd if="$f" bs=1 count=4 2>/dev/null)" = "$ELF_MAGIC" ] && TEST_OBJS+=("$f")
    done < <(find "${TESTS_ROOT}/opt/quantum/chimera/tests" -type f 2>/dev/null)

    if [ "${#TEST_OBJS[@]}" -eq 0 ]; then
        echo "ERROR: no ELF objects found under ${TESTS_ROOT}" >&2
        exit 1
    fi

    WORK="$(mktemp -d)"
    mkdir -p "${WORK}/debian"
    cat > "${WORK}/debian/control" <<EOF
Source: quantum-chimera-tests

Package: quantum-chimera-tests
Architecture: ${ARCH}
EOF
    RAW_DEPS="$(cd "${WORK}" && \
        dpkg-shlibdeps -O --ignore-missing-info -l"${LIBDIR}" "${TEST_OBJS[@]}")"
    rm -rf "${WORK}"
    TESTS_SHLIB_DEPS="$(printf '%s\n' "${RAW_DEPS}" | sed -n 's/^shlibs:Depends=//p')"

    if [ -z "${TESTS_SHLIB_DEPS}" ]; then
        echo "ERROR: dpkg-shlibdeps produced no dependencies for the tests package" >&2
        exit 1
    fi

    TESTS_DEPENDS="quantum-chimera-server (= ${VERSION}-${REVISION}), ${TESTS_SHLIB_DEPS}"

    mkdir -p "${TESTS_ROOT}/DEBIAN"
    sed -e "s/__VERSION__/${VERSION}/" \
        -e "s/__REVISION__/${REVISION}/" \
        -e "s/__ARCH__/${ARCH}/" \
        -e "s/__COMMIT__/${COMMIT}/" \
        -e "s|__DEPENDS__|${TESTS_DEPENDS}|" \
        /context/qtm_build/debian/control-tests.tmpl > "${TESTS_ROOT}/DEBIAN/control"

    dpkg-deb --build ${TESTS_PKG_DIR}
    mv ${TESTS_PKG_DIR}.deb /dist/
fi

# Make the chimera Admin python module
# use a Virtual env to have the correct
# python packages for the build
echo "Building the python3-chimera-admin package"
python3 -m venv  /build/venv
source /build/venv/bin/activate
pip3 install 'setuptools>=61' 'packaging>=24.2' build
cp -r /context/src/admin /build
cd /build/admin
export PYBUILD_INTERPRETERS=python3
export DEB_BUILD_OPTIONS=nocheck
dpkg-buildpackage -d -us -uc -b
mv ../python3-chimera-admin*.deb /dist

cd /dist
tar zcvf repo-chimera.tar.gz *.deb

