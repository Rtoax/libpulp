#
# spec file for package libpulp
#
# Copyright (c) 2018 SUSE LINUX Products GmbH, Nuernberg, Germany.
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

Name:           libfibo_livepatch
Version:	0.1
Release:	0
License:	SUSE-GPL-2.0
Summary:	Livepatch for libfibo
Source0:	%{name}-%{version}.tar.bz2

%define topdir %(echo $PWD)/rpmbuild
%define _builddir %{topdir}/build
%define _rpmdir %{topdir}/rpms
%define _sourcedir %(echo $PWD)
%define _specdir %(echo $PWD)
%define _srcrpmdir %{topdir}/rpms
%define patchdir /var/ulp/%{name}-%{version}

BuildRequires:	fiboapp = 0.1
BuildRequires:	libpulp
Requires:	libpulp
Requires:	lua53

%description
This package provides a livepatch for libfibo.

%prep
%setup -q -n libfibo_livepatch-%{version}

%build
./build-livepatch.sh

%install
mkdir -p %{buildroot}%{patchdir}
mv ./build/patch1.so %{buildroot}%{patchdir}/patch1.so
mv ./build/metadata1.ulp %{buildroot}%{patchdir}/metadata1.ulp
mv ./build/reverse1.ulp %{buildroot}%{patchdir}/reverse1.ulp

%post -n libfibo_livepatch
lua %{_bindir}/ulp_dispatcher patch %{patchdir}/metadata1.ulp

%preun
lua %{_bindir}/ulp_dispatcher patch %{patchdir}/reverse1.ulp

%files -n libfibo_livepatch
%defattr(-,root,root)
%{patchdir}/patch1.so
%{patchdir}/metadata1.ulp
%{patchdir}/reverse1.ulp
