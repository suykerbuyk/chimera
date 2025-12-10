# SPDX-FileCopyrightText: 2025 Ben Jarvis
#
# SPDX-License-Identifier: Unlicense

FROM repository.mdh.quantum.com/ubuntu:22.04 AS build
ARG BUILD_TYPE=Release


COPY sources.list /etc/apt/sources.list

RUN apt-get -y update && \
    apt-get -y --no-install-recommends upgrade && \
    apt-get -y --no-install-recommends install gcc g++ cmake ninja-build \
            git flex bison uuid-dev uthash-dev libkrb5-3 libkrb5-dev libgssapi-krb5-2 \
            gss-ntlmssp-dev librdmacm-dev libjansson-dev libxxhash-dev liburcu-dev \
            libunwind-dev libssl-dev openssl libnuma-dev curl ca-certificates \
            make libcurl4-openssl-dev libnfs-dev libs3-dev iproute2 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# install uncrustify version 0.78
RUN curl -o /usr/local/bin/uncrustify  ${asset:uncrustify:Ubuntu22/$arch/uncrustify} && \
    chmod +x /usr/local/bin/uncrustify
