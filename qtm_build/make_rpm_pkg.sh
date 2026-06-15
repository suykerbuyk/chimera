#!/bin/bash
#
# Script that create the chimera PKG in a QB build
# container
#

VERSION=$1
REVISION=$2
ARCH=$3

mkdir -p /build /dist /repo
cd /build
echo "%_topdir /build/rpmbuild" > $HOME/.rpmmacros
rpmdev-setuptree
cp /dist/chimera.tar.gz /build/rpmbuild/SOURCES

rpmbuild -ba \
    --define "version ${VERSION}" \
    --define "release ${REVISION}" \
    /context/qtm_build/chimera.spec

# Make the chimera Admin python module
# use a Virtual env to have the correct
# python packages for the build
echo "Building the python3-chimera-admin package"
python3 -m venv /build/venv
source /build/venv/bin/activate
pip3 install 'setuptools>=61' 'packaging>=24.2' build
cp -r /context/src/admin /build/admin
pip3 install --no-deps --no-build-isolation --no-compile \
    --root /build/admin-root --prefix /usr /build/admin
deactivate
tar -czf /build/rpmbuild/SOURCES/chimera-admin.tar.gz -C /build/admin-root .

rpmbuild -ba \
    --define "version ${VERSION}" \
    --define "release ${REVISION}" \
    /context/qtm_build/python3-chimera-admin.spec

# Create a repo with prereq RPM.
cp /build/rpmbuild/RPMS/x86_64/*.rpm /repo
cp /build/rpmbuild/RPMS/noarch/*.rpm /repo
dnf download xxhash-libs libunwind liburing --arch  ${ARCH} --destdir /repo
cd /repo
tar cvfz /dist/repo-chimera.tar.gz .

