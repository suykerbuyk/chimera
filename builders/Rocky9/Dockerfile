#FROM repository.mdh.quantum.com/rockylinux:9
FROM rockylinux:9

COPY packages.txt /install/packages.txt

# install required packages
RUN dnf update -y && \
    dnf install -y dnf-plugins-core epel-release && \
    dnf config-manager --set-enabled devel && \
    dnf config-manager --set-enabled crb && \
    dnf install --allowerasing -y $(cat /install/packages.txt)

# install uncrustify version 0.78
RUN curl -o /usr/local/bin/uncrustify  ${asset:uncrustify:Rocky9/$arch/uncrustify} && \
    chmod +x /usr/local/bin/uncrustify

# install Stornext dev packages.
# TODO re-enable
#RUN cd /install && \
#    curl https://nexus.mdh.quantum.com/repository/snnas-binaries/snfs/snfs-7.2.4-OS9.tar -O && \
#    tar xf snfs-*-OS9.tar && \
#    dnf install -y /install/OS9/*.rpm


# Install gss-ntlmssp 1.3.1 keep copy for build
RUN mkdir -p /packages && \
    curl -sS ${asset:gss-ntlmssp:Rocky9/$arch/gss-ntlmssp-1.3.1.tgz} | tar zxvf - -C /packages && \
    dnf install -y /packages/*

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
