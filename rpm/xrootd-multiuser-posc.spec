Name: xrootd-multiuser-posc
Version: 0.1.0
Release: 1%{?dist}
Summary: Persist-on-successful-close (rename-on-close) OSS plugin for xrootd-multiuser

License: BSD
URL: https://github.com/geonmo/xrootd-multiuser-posc
# git archive v%{version} --prefix=%{name}-%{version}/ | gzip -7 > ~/rpmbuild/SOURCES/%{name}-%{version}.tar.gz
Source0: %{name}-%{version}.tar.gz

%define xrootd_current_major 5
%define xrootd_next_major 6

BuildRequires: xrootd-server-devel >= 1:%{xrootd_current_major}
BuildRequires: xrootd-server-devel <  1:%{xrootd_next_major}
BuildRequires: cmake
BuildRequires: gcc-c++
BuildRequires: libcap-devel

Requires: xrootd-server >= 1:%{xrootd_current_major}
Requires: xrootd-server <  1:%{xrootd_next_major}.0.0-1
# Close() and the other handle operations must run as the mapped user,
# which xrootd-multiuser does from the KISTI-GSDC 2.2.1-1.1.2 site build on.
Requires: xrootd-multiuser >= 2.2.1-1.1.2

%description
OSS plugin stacked below xrootd-multiuser.  New files are written under a
hidden temporary name and moved to their final name only when the upload is
complete; incomplete uploads are removed, and uploads interrupted by a server
stop are cleaned up at the next startup from a journal.

%prep
%setup -q

%build
%cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
%cmake_build

%install
%cmake_install
install -d -m 1733 %{buildroot}%{_localstatedir}/spool/xrootd/posc-journal

%files
%doc README.md
%{_libdir}/libXrdMultiuserPosc-5.so
%dir %attr(1733,xrootd,xrootd) %{_localstatedir}/spool/xrootd/posc-journal

%changelog
* Thu Oct 22 2026 Geonmo Ryu <geonmo@kisti.re.kr> - 0.1.0-1
- Initial package: rename-on-close POSC emulation for xrootd-multiuser
