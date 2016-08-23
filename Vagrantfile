# -*- mode: ruby -*-
# vi: set ft=ruby :

$script = <<SCRIPT
  # General packages.
  apt-get update -q
  apt-get install -qq unzip apt-transport-https \
    autotools-dev automake libtool python-docutils pkg-config libpcre3-dev \
    libeditline-dev libedit-dev make dpkg-dev libcurl4-openssl-dev

  # Varnish Cache.
  curl https://repo.varnish-cache.org/debian/GPG-key.txt | apt-key add -
  echo "deb https://repo.varnish-cache.org/ubuntu/ trusty varnish-4.1" > /etc/apt/sources.list.d/varnish-cache.list
  apt-get update -q
  apt-get install -qq varnish libvarnishapi-dev
  sudo cp /usr/local/share/aclocal/varnish.m4 /usr/share/aclocal/

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
  config.vm.synced_folder '.', '/vagrant', :nfs => false
  config.vm.provider :virtualbox do |vb|
    vb.customize [
      'modifyvm', :id,
      '--memory', '1024',
      '--natdnshostresolver1', 'on',
      '--accelerate3d', 'off',
    ]
  end

  config.vm.define :v41 do |machine|
    machine.vm.box = 'ubuntu/trusty64'
    machine.vm.box_version = '=14.04'
    machine.vm.box_check_update = true
    machine.vm.provision :shell, :privileged => true, :keep_color => false, :inline => $script
    machine.vm.provider :virtualbox do |vb|
      vb.customize [
        'modifyvm', :id,
        '--name', 'libvmod-cfg (Varnish 4.1.x)',
      ]
    end
  end
end
