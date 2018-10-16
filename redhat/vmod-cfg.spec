Summary: Config VMOD for Varnish
Name: vmod-cfg
Version: 5.3
Release: 1%{?dist}
License: BSD
URL: https://github.com/carlosabalde/libvmod-cfg
Group: System Environment/Daemons
Source0: libvmod-cfg.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: varnish >= 6.0.0, libcurl, luajit
BuildRequires: make, python-docutils, varnish >= 6.0.0, varnish-devel >= 6.0.0, libcurl-devel, luajit-devel

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
* Sat Oct 13 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.3-1.20181013
- Added new parameter 'prefix' to .dump() methods.
- Linked against LuaJIT when possible.
- Added new parameter 'gc_collect' to .execute() method.
* Tue Aug 07 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.2-1.20180807
- Increased INI_MAX_LINE from 2KB to 16KB
* Wed Jun 27 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.1-1.20180627
- Added cfg.rules().
- Added cfg.script().
- Added stream option to .dump() methods.
* Thu Mar 15 2018 Carlos Abalde <carlos.abalde@gmail.com> - 5.0-1.20180315
- Migrated to Varnish Cache 6.0.x.
* Fri Sep 15 2017 Carlos Abalde <carlos.abalde@gmail.com> - 4.0-1.20170915
- Migrated to Varnish Cache 5.2.x.
* Fri Aug 25 2017 Carlos Abalde <carlos.abalde@gmail.com> - 3.1-1.20170825
- Added JSON support.
* Fri Mar 17 2017 Carlos Abalde <carlos.abalde@gmail.com> - 3.0-1.20170317
- Migrated to Varnish Cache 5.1.x.
* Fri Mar 17 2017 Carlos Abalde <carlos.abalde@gmail.com> - 2.0-1.20170317
- Migrated to Varnish Cache 5.0.x.
* Thu Feb 02 2017 Carlos Abalde <carlos.abalde@gmail.com> - 1.3-1.20170202
- Fixed bug reading files from disk.
- Added reporting of error code when failed to parse .ini files.
- Updated .ini parser.
* Fri Sep 16 2016 Carlos Abalde <carlos.abalde@gmail.com> - 1.2-1.20160916
- Initial version.
