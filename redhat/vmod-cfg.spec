Summary: Config VMOD for Varnish
Name: vmod-cfg
Version: 1.9
Release: 1%{?dist}
License: BSD
URL: https://github.com/carlosabalde/libvmod-cfg
Group: System Environment/Daemons
Source0: libvmod-cfg.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: varnish >= 4.1.0, libcurl, luajit
BuildRequires: make, python-docutils, varnish >= 4.1.0, varnish-devel >= 4.1.0, libcurl-devel, luajit-devel, jemalloc-devel

%description
Config VMOD for Varnish

%prep
%setup -n libvmod-cfg

%build
./autogen.sh
./configure --prefix=/usr/ --docdir='${datarootdir}/doc/%{name}' --libdir='%{_libdir}'
%{__make}
%{__make} check

%install
[ %{buildroot} != "/" ] && %{__rm} -rf %{buildroot}
%{__make} install DESTDIR=%{buildroot}

%clean
[ %{buildroot} != "/" ] && %{__rm} -rf %{buildroot}

%files
%defattr(-,root,root,-)
%{_libdir}/varnish*/vmods/
%doc /usr/share/doc/%{name}/*
%{_mandir}/man?/*

%changelog
* Thu Oct 25 2018 Carlos Abalde <carlos.abalde@gmail.com> - 1.9-1.20181025
- Triggered 'thread.tcache.flush' also on LuaJIT executions.
- Added Lua / LuaJIT memory consumption to stats.
- Added Lua 5.2 & 5.3 support to build system.
* Tue Oct 16 2018 Carlos Abalde <carlos.abalde@gmail.com> - 1.8-1.20181016
- Flushed 'thread.tcache.flush' on Lua (not LuaJIT) script executions.
* Sat Oct 13 2018 Carlos Abalde <carlos.abalde@gmail.com> - 1.7-1.20181013
- Added new parameter 'prefix' to .dump() methods.
- Linked against LuaJIT when possible.
- Added new parameter 'gc_collect' to .execute() method.
* Tue Aug 07 2018 Carlos Abalde <carlos.abalde@gmail.com> - 1.6-1.20180807
- Increased INI_MAX_LINE from 2KB to 16KB
* Wed Jun 27 2018 Carlos Abalde <carlos.abalde@gmail.com> - 1.5-1.20180627
- Added cfg.rules().
- Added cfg.script().
- Added stream option to .dump() methods.
* Fri Aug 25 2017 Carlos Abalde <carlos.abalde@gmail.com> - 1.4-1.20170825
- Added JSON support.
* Thu Feb 02 2017 Carlos Abalde <carlos.abalde@gmail.com> - 1.3-1.20170202
- Fixed bug reading files from disk.
- Added reporting of error code when failed to parse .ini files.
- Updated .ini parser.
* Fri Sep 16 2016 Carlos Abalde <carlos.abalde@gmail.com> - 1.2-1.20160916
- Initial version.
