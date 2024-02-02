FROM ubuntu:focal-20211006

ENV DEBIAN_FRONTEND noninteractive

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
        libeditline-dev \
        libjemalloc-dev \
        liblua5.1-0-dev \
        libluajit-5.1-dev \
        libncurses-dev \
        libpcre3-dev \
        libtool \
        lua5.1 \
        luajit \
        make \
        nano \
        netcat \
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
    && ln -fs /usr/bin/python3.8 /usr/bin/python3 \
    && ln -fs /usr/bin/python3.8 /usr/bin/python \
    && apt clean \
    && rm -rf /var/lib/apt/lists/*

RUN cd /tmp \
    && wget --no-check-certificate https://varnish-cache.org/_downloads/varnish-4.1.10.tgz \
    && tar zxvf varnish-*.tgz \
    && rm -f varnish-*.tgz \
    && cd varnish-* \
    && ./autogen.sh \
    && ./configure \
    && make \
    && make PREFIX='/usr/local' install \
    && ldconfig

COPY ./docker-entrypoint.sh /
ENTRYPOINT ["/docker-entrypoint.sh"]
