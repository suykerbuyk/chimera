FROM repository.mdh.quantum.com/ubuntu:26.04
ARG BUILD_TYPE=Release
ENV DEBIAN_FRONTEND noninteractive
ENV APT_MIRROR=http://repository.mdh.quantum.com/repository/pkgs/ubuntu

#Use Quantum internal proxy repo
RUN echo "deb $APT_MIRROR resolute main universe" > /etc/apt/sources.list.d/local-mirror.list && \
    echo "deb $APT_MIRROR resolute-updates main universe" >> /etc/apt/sources.list.d/local-mirror.list && \
    echo "deb $APT_MIRROR resolute-security main universe" >> /etc/apt/sources.list.d/local-mirror.list && \
    rm -f /etc/apt/sources.list.d/ubuntu.sources && \
    ls -l /etc/apt/sources.list.d/

RUN apt-get -y update && \
    apt-get -y --no-install-recommends upgrade && \
    apt-get -y --no-install-recommends install gcc g++ cmake ninja-build git flex \
            bison uuid-dev uthash-dev libkrb5-3 libkrb5-dev libgssapi-krb5-2  \
            librdmacm-dev libjansson-dev libxxhash-dev liburcu-dev libunwind-dev libssl-dev \
            openssl libnuma-dev curl ca-certificates make libcurl4-openssl-dev libnfs-dev \
	    iproute2 libs3-dev python3 python3-pip python3-venv python3-requests pkg-config \
	    libwbclient-dev

# install uncrustify version 0.78
RUN curl -o /usr/local/bin/uncrustify  ${asset:uncrustify:Ubuntu26/$arch/uncrustify} && \
    chmod +x /usr/local/bin/uncrustify

# Cleanup build cache
RUN apt-get clean && rm -rf /var/lib/apt/lists/*
