# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # General packages.
  apt-get update -q
  apt-get install -qq unzip apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev git libjemalloc-dev \
    libncurses-dev python-sphinx graphviz libcurl3 libcurl4-gnutls-dev \
    lua5.1 liblua5.1-0-dev luajit libluajit-5.1-dev vim-common

  # Varnish Cache.
  sudo -u vagrant bash -c '\
    wget --no-check-certificate https://varnish-cache.org/_downloads/varnish-6.5.0.tgz; \
    tar zxvf varnish-*.tgz; \
    rm -f varnish-*.tgz; \
    cd varnish-*; \
    ./autogen.sh; \
    ./configure; \
    make; \
    sudo make PREFIX="/usr/local" install; \
    sudo ldconfig'

  # VMOD.
  sudo -u vagrant bash -c '\
    cd /vagrant; \
    ./autogen.sh; \
    ./configure; \
    make'
SCRIPT

Vagrant.configure('2') do |config|
  config.vm.hostname = 'dev'
  config.vm.network :public_network
  config.vm.synced_folder '.', '/vagrant', type: 'virtualbox'
  config.vm.provider :virtualbox do |vb|
    vb.memory = 1024
    vb.cpus = 1
    vb.linked_clone = true
    vb.customize [
      'modifyvm', :id,
      '--natdnshostresolver1', 'on',
      '--natdnsproxy1', 'on',
      '--accelerate3d', 'off',
      '--audio', 'none',
      '--paravirtprovider', 'Default',
    ]
  end

  config.vm.define :v65 do |machine|
    machine.vm.box = 'ubuntu/bionic64'
    machine.vm.box_version = '=20210129.0.0'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-cfg (Varnish 6.5.x)',
      ]
    end
  end
end
