FROM ubuntu:noble-20260410

ENV DEBIAN_FRONTEND=noninteractive

RUN groupadd -g 5000 dev \
    && useradd -u 5000 -g 5000 -m -s /bin/bash dev

RUN apt update \
    && apt install -y \
        apt-transport-https \
        automake \
        autotools-dev \
        bindfs \
        binutils \
        curl \
        dpkg-dev \
        git \
        gpg \
        graphviz \
        jq \
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
    && ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
