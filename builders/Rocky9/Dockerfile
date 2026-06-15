FROM repository.mdh.quantum.com/rockylinux/rockylinux:9

# install required packages
RUN dnf install -y dnf-plugins-core epel-release && \
    dnf config-manager --set-enabled crb

RUN dnf -y update && \
    dnf -y --allowerasing install gcc gcc-c++ clang clang-tools-extra cmake ninja-build git flex bison llvm \
    libuuid-devel uthash-devel krb5-devel krb5-libs \
    rdma-core-devel jansson-devel xxhash-devel userspace-rcu-devel liburing-devel libunwind-devel libasan \
    rocksdb-devel openssl-devel openssl numactl-devel libcurl-devel libaio-devel \
    autoconf automake libtool pkgconfig ca-certificates uncrustify clang-analyzer iproute curl libnl3-devel \
    libwbclient-devel samba-client krb5-server krb5-workstation samba samba-winbind samba-winbind-clients \
    python3-boto3 python3-pip which \
    rpm-build rpm-sign rpmdevtools sudo


# install uncrustify version 0.78
RUN curl -o /usr/local/bin/uncrustify  ${asset:uncrustify:Rocky9/$arch/uncrustify} && \
    chmod +x /usr/local/bin/uncrustify

# install Stornext dev packages.
RUN mkdir -p /install && cd /install && \
    curl https://nexus.mdh.quantum.com/repository/snnas-binaries/snfs/snfs-7.2.4-OS9.tar -O && \
    tar xf snfs-*-OS9.tar && \
    dnf install -y /install/OS9/*.rpm


# Final cleanup
RUN rm -rf /install && \
    rm -rf /var/cache/yum && \
    mkdir -p --mode=0755 /var/cache/yum && \
    dnf clean all

