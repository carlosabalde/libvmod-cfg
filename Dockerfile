FROM ubuntu:resolute-20260421

ARG VCC=gcc

ENV DEBIAN_FRONTEND=noninteractive

RUN groupadd -g 5000 dev \
    && useradd -u 5000 -g 5000 -m -s /bin/bash dev

RUN apt update \
    && apt install -y \
        apt-transport-https \
        automake \
        autoconf-archive \
        autotools-dev \
        bindfs \
        binutils \
        clang \
        cpio \
        curl \
        dpkg-dev \
        furo \
        git \
        gpg \
        graphviz \
        jq \
        lcov \
        less \
        libcurl4-gnutls-dev \
        libedit-dev \
        libjemalloc-dev \
        liblua5.1-0-dev \
        libluajit-5.1-dev \
        libncurses-dev \
        libpcre2-dev \
        libssl-dev \
        libtool \
        lua5.1 \
        luajit \
        make \
        nano \
        netcat-traditional \
        pkg-config \
        python3 \
        python3-docutils \
        python3-sphinx \
        python3-venv \
        tar \
        telnet \
        unzip \
        vim-common \
        wget \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/varnish/varnish.git /tmp/varnish \
    && cd /tmp/varnish \
    && git submodule update --init \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN git clone --recursive https://code.vinyl-cache.org/vinyl-cache/vinyl-cache /tmp/vinyl-cache \
    && cd /tmp/vinyl-cache \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
