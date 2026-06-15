Name: quantum-chimera-server
Version: %{version}
Release: %{release}

Summary: Quantum Chimera NAS server
License: Quantum Corporation
Packager: Quantum Corporation
Vendor: Quantum Corporation
URL: http://www.quantum.com/

Source0: chimera.tar.gz

Requires: redhat-release >= 9
Requires: redhat-release < 10
Requires: xxhash-libs
Requires: libunwind
Requires: liburing
Requires: openssl
Requires: python3
Requires: python3-requests

%description
Quantum build of the Chimera NAS server for
the StorNext Filesystem (SNFS)


%build
# nothing to build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}
mkdir -p %{buildroot}/etc/ld.so.conf.d/
echo "/opt/quantum/chimera/lib" > %{buildroot}/etc/ld.so.conf.d/chimera.conf


%post
%systemd_post chimera.service
ln -s -f /opt/quantum/chimera/etc/chimera.json /etc/chimera
ldconfig
exit 0

%postun
ldconfig
exit 0

%files
%defattr(-,root,root)
/usr/lib/systemd/system/chimera.service
/etc/ld.so.conf.d/chimera.conf
%dir /etc/chimera
%config(noreplace) /etc/default/chimera
%config(noreplace) /etc/chimera/suppressions.txt
%dir /opt/quantum/chimera
/opt/quantum/chimera/bin
/opt/quantum/chimera/include
/opt/quantum/chimera/lib
/opt/quantum/chimera/sbin
%doc /opt/quantum/chimera/share/*
%config(noreplace) /opt/quantum/chimera/etc/chimera.json
%ghost /etc/chimera/chimera.json
