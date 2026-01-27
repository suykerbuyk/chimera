# TODO: fix internal repo to allow arm build
# FROM repository.mdh.quantum.com/ubuntu:24.04 AS build
FROM ubuntu:24.04 AS build
ARG BUILD_TYPE=Release
ENV DEBIAN_FRONTEND noninteractive

COPY sources.list /etc/apt/sources.list

RUN apt-get -y update && \
    apt-get -y --no-install-recommends upgrade && \
    apt-get -y --no-install-recommends install gcc g++ cmake ninja-build git flex \
            bison uuid-dev uthash-dev libkrb5-3 libkrb5-dev libgssapi-krb5-2  gss-ntlmssp-dev \
            librdmacm-dev libjansson-dev libxxhash-dev liburcu-dev libunwind-dev libssl-dev \
            openssl libnuma-dev curl ca-certificates make libcurl4-openssl-dev libnfs-dev \
	    iproute2 libs3-dev

# install uncrustify version 0.78
RUN curl -o /usr/local/bin/uncrustify  ${asset:uncrustify:Ubuntu24/$arch/uncrustify} && \
    chmod +x /usr/local/bin/uncrustify

# Install gss-ntlmssp 1.3.1 keep copy for build
RUN mkdir -p /packages && \
    curl -sS ${asset:gss-ntlmssp:Ubuntu24/$arch/gss-ntlmssp-1.3.1.tgz} | tar zxvf - -C /packages && \
    apt install -y --no-install-recommends /packages/*

# Cleanup build cache
RUN apt-get clean && rm -rf /var/lib/apt/lists/*
