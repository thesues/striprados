Name:		striprados	
Version:	%{ver}
Release:	%{rel}%{?dist}
Summary:	a command line tool to upload/download striped rados object

Group:		System Environment/Base
License:	GPL
URL:		http://10.150.130.22:22222
Source0:	%{name}-%{version}-%{rel}.tar.gz
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:	ceph-devel
Requires:	libradosstriper1
Requires:	librados2

%description
wrap radosstriper API to upload/download/delete/list rados cluster storage.


%prep
%setup -q -n %{name}-%{version}-%{rel}

%build
make %{?_smp_mflags}


%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}


%clean
rm -rf %{buildroot}


%files
%defattr(-,root,root,-)
%{_bindir}/striprados
%doc



%changelog

