# Git commit id the package was built from; passed in by make_rpm_pkg.sh
# via --define "commit <sha>".  Query with rpm -q --qf '%%{VCS}\n'.
%{!?commit: %define commit unknown}

Name: python3-chimera-admin
Version: %{version}
Release: %{release}
Vcs: %{commit}

Summary: Chimera admin Python SDK
License: Quantum Corporation
Packager: Quantum Corporation
Vendor: Quantum Corporation
URL: http://www.quantum.com/

BuildArch: noarch

Source0: chimera-admin.tar.gz

Requires: python3
Requires: python3-requests

%description
Python SDK for the Chimera NAS REST API.


%build
# nothing to build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
tar -xzf %{SOURCE0} -C %{buildroot}


%files
%defattr(-,root,root)
/usr/lib/python3*/site-packages/*
