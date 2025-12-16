#!/bin/bash -e

VERSION=$1
REVISION=$2
ARCH=$3

PKG_DIR="quantum-chimera-server_${VERSION}-${REVISION}_${ARCH}"

mkdir -p /build /dist
cd /build
mkdir -p ${PKG_DIR}
cd ${PKG_DIR}
mkdir -p DEBIAN lib/systemd/system
cp /context/qtm_build/debian/* DEBIAN/
sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__REVISION__/${REVISION}/" \
    -e "s/__ARCH__/${ARCH}/" \
    /context/qtm_build/debian/control.tmpl > DEBIAN/control
cp /context/qtm_build/chimera.service lib/systemd/system
tar xf /dist/chimera.tar.gz
cd ..
dpkg-deb --build ${PKG_DIR}
tar zcvf /dist/repo-chimera.tar.gz ${PKG_DIR}.deb
cp ${PKG_DIR}.deb /dist/

