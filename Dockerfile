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

RUN cd /tmp \
    && wget --no-check-certificate https://github.com/varnish/varnish/releases/download/varnish-9.0.0/varnish-9.0.0.tar.gz \
    && tar zxvf varnish-*.tar.gz \
    && rm -f varnish-*.tar.gz \
    && cd varnish-* \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

RUN cd /tmp \
    && wget --no-check-certificate https://vinyl-cache.org/downloads/vinyl-cache-9.0.1.tgz \
    && tar zxvf vinyl-cache-9.0.1.tgz \
    && rm -f vinyl-cache-9.0.1.tgz \
    && cd vinyl-cache-9.0.1 \
    && ./autogen.sh \
    && CC="${VCC}" ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
