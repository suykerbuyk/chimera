#!/bin/bash -e

VERSION=$1
REVISION=$2
ARCH=$3

PKG_DIR="$(basename $PWD)"

mkdir -p ./DEBIAN
cp -r /context/qtm_build/debian/* ./DEBIAN/
sed -e "s/__VERSION__/${VERSION}/" \
    -e "s/__REVISION__/${REVISION}/" \
    -e "s/__ARCH__/${ARCH}/" \
    ./DEBIAN/control.tmpl > ./DEBIAN/control
mkdir -p etc/ld.so.conf.d/
echo "/opt/quantum/chimera/lib" > etc/ld.so.conf.d/chimera.conf

cd ..
dpkg-deb --build ${PKG_DIR}
cp ${PKG_DIR}.deb /dist/
cd /dist
tar zcvf repo-chimera.tar.gz *.deb

