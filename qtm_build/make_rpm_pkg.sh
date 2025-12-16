#!/bin/bash
#
# Script that create the chimera PKG in a QB build
# container
#

VERSION=$1
REVISION=$2
ARCH=$3

mkdir -p /build /dist /repo
c /build
rpmdev-setuptree
cp /dist/chimera.tar.gz /root/rpmbuild/SOURCES
cp /context/qtm_build/chimera.service /root/rpmbuild/SOURCES

sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__REVISION__/${REVISION}/" \
    /context/qtm_build/chimera.spec.tmpl > chimera.spec
rpmbuild -ba  chimera.spec

# Create a repo with prereq RPM.
cp /root/rpmbuild/RPMS/x86_64/*.rpm /repo
dnf download xxhash-libs libunwind liburing --arch  ${ARCH} --destdir /repo
cd /repo
tar cfz /dist/repo-chimera.tar.gz .

