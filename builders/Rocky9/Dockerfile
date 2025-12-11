#FROM repository.mdh.quantum.com/rockylinux:9
FROM rockylinux:9

COPY packages.txt /install/packages.txt

# install required packages
RUN dnf update -y && \
    dnf install -y dnf-plugins-core epel-release && \
    dnf config-manager --set-enabled devel && \
    dnf config-manager --set-enabled crb && \
    dnf install --allowerasing -y $(cat /install/packages.txt)

# install Stornext dev packages.
RUN cd /install && \
    curl https://nexus.mdh.quantum.com/repository/snnas-binaries/snfs/snfs-7.2.4-OS9.tar -O && \
    tar xf snfs-*-OS9.tar && \
    dnf install -y /install/OS9/*.rpm

# package and install gssntlmssp
ENV PKG_SRC https://download.fedoraproject.org/pub/fedora/linux/development/rawhide/Everything/source/tree/Packages/g/gssntlmssp-1.3.1-5.fc43.src.rpm
#ENV PKG_SRC https://dl.rockylinux.org/pub/rocky/8/Devel/source/tree/Packages/g/gssntlmssp-1.2.0-1.el8_8.src.rpm
RUN cd && rpmdev-setuptree && \
    rpm -ivh ${PKG_SRC} && \
    rpmbuild -ba ~/rpmbuild/SPECS/gssntlmssp.spec && \
    rpm -ivh rpmbuild/RPMS/x86_64/gssntlmssp-[0-9]* && \
    rpm -ivh rpmbuild/RPMS/x86_64/gssntlmssp-devel-[0-9]*


# Make copy for install script
RUN mkdir /repo && \
    cp /root/rpmbuild/RPMS/x86_64/gssntlmssp-[0-9]* /repo/ && \
    rm -rf ./rpmbuild

RUN useradd -d /context -ms /bin/bash mockbuild
WORKDIR /context
RUN chown -R mockbuild /context


# Final cleanup
RUN rm -rf /install && \
    rm -rf /var/cache/yum && \
    mkdir -p --mode=0755 /var/cache/yum && \
    yum clean all

# Grab dumb-init
RUN curl -sS -o /usr/bin/dumb-init https://nexus.mdh.quantum.com/repository/snnas-binaries/dumb-init/dumb-init_1.2.5_x86_64
RUN chmod 755 /usr/bin/dumb-init

ENTRYPOINT ["/usr/bin/dumb-init", "--"]
