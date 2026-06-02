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
mv ${PKG_DIR}.deb /dist/

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

