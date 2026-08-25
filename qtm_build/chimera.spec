# Git commit id the package was built from; passed in by make_rpm_pkg.sh
# via --define "commit <sha>".  Query with rpm -q --qf '%%{VCS}\n'.
%{!?commit: %define commit unknown}

Name: quantum-chimera-server
Version: %{version}
Release: %{release}
Vcs: %{commit}

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
Requires: python3
Requires: python3-requests

%description
Quantum build of the Chimera NAS server.

%package -n quantum-chimera-snfs-module
Summary: Quantum Chimera StorNext (SNFS) VFS module
Requires: quantum-chimera-server = %{version}-%{release}

%description -n quantum-chimera-snfs-module
StorNext (SNFS) VFS backend module for the Quantum Chimera NAS server.
This module links against the StorNext External API (libcvextapi) and
therefore requires StorNext (cvfs) to be installed. Packaging it separately
lets quantum-chimera-server install on nodes that do not have cvfs.

%package -n quantum-chimera-tests
Summary: Quantum Chimera POSIX validation test suite
Requires: quantum-chimera-server = %{version}-%{release}

%description -n quantum-chimera-tests
Chimera validation test suites installed under /opt/quantum/chimera/tests:
POSIX test programs (chimera POSIX suite, cthon and pjdfstest ports) and
client API tests, which run a Chimera stack in-process against a
selectable VFS backend (-b); SMB server tests with the WPTS/pike
conformance fixtures and wrapper scripts; and fio/elbencho benchmark
configs for the installed plugin libraries. These let a deployed system
validate VFS modules such as snfs without a development tree.


%build
# nothing to build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}
mkdir -p %{buildroot}/etc/ld.so.conf.d/
echo "/opt/quantum/chimera/lib" > %{buildroot}/etc/ld.so.conf.d/chimera.conf
# Version stamp for quick reference on installed systems; the same
# information is carried in the VCS header tag.  Deliberately not
# %%config: it must be replaced on every upgrade.
echo "version: %{version}-%{release}" >  %{buildroot}/opt/quantum/chimera/etc/version
echo "commit: %{commit}"              >> %{buildroot}/opt/quantum/chimera/etc/version
# Runtime state directory (CHIMERA_STATE_DIR): holds secrets persisted by the
# server (NFS file-handle signing key, SMB server GUID), so keep it 0700.
mkdir -p %{buildroot}/opt/quantum/chimera/share/state


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
/opt/quantum/chimera/etc/version
%dir /etc/chimera
%config(noreplace) /etc/default/chimera
%config(noreplace) /etc/chimera/suppressions.txt
%dir /opt/quantum/chimera
/opt/quantum/chimera/bin
/opt/quantum/chimera/include
/opt/quantum/chimera/lib
%exclude /opt/quantum/chimera/lib/libchimera_vfs_snfs.so
/opt/quantum/chimera/sbin
# share/ entries are listed explicitly rather than globbed so that
# share/state is not swept up as %%doc (an --excludedocs install would
# then not create it).
%doc /opt/quantum/chimera/share/examples
%doc /opt/quantum/chimera/share/man
%dir %attr(0700,root,root) /opt/quantum/chimera/share/state
%exclude /opt/quantum/chimera/share/examples/chimera-snfs.json
%config(noreplace) /opt/quantum/chimera/etc/chimera.json
%ghost /etc/chimera/chimera.json

%files -n quantum-chimera-snfs-module
%defattr(-,root,root)
/opt/quantum/chimera/lib/libchimera_vfs_snfs.so
%doc /opt/quantum/chimera/share/examples/chimera-snfs.json

%files -n quantum-chimera-tests
%defattr(-,root,root)
/opt/quantum/chimera/tests
