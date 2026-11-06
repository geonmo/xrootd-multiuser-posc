#!/bin/bash
# Build the RPM and SRPM inside an AlmaLinux 9 container (GitHub Actions or podman).
#   SPEC            spec file path relative to the repository root
#   DIST            %{dist} value, e.g. .osg25.el9
#   XROOTD_VERSION  xrootd devel packages to build against (epoch 1), e.g. 5.9.7-1.2.osg25.el9
#                   KISTI-GSDC runs xrootd 5.9.7-1.2.1 (OSG 1.2 + a .cc-only patch, same headers),
#                   so plugins are built against the OSG 1.2 devel packages.
#   OUT             directory to copy the built RPMs into (default: dist/)
# Usage: rpm/ci-build.sh   (run from the repository root, git checkout of the tag to build)
set -euo pipefail
: "${SPEC:?}" "${DIST:?}" "${XROOTD_VERSION:?}"
OUT=${OUT:-dist}

dnf -y -q install epel-release dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -y -q install https://repo.opensciencegrid.org/osg/25-main/osg-25-main-el9-release-latest.rpm
dnf -y -q install git rpm-build rpmdevtools gzip \
  "xrootd-devel-1:${XROOTD_VERSION}" "xrootd-server-devel-1:${XROOTD_VERSION}" \
  "xrootd-private-devel-1:${XROOTD_VERSION}" "xrootd-server-libs-1:${XROOTD_VERSION}"
git config --global --add safe.directory "$PWD"

name=$(rpmspec -q --srpm --qf '%{name}' "$SPEC")
version=$(rpmspec -q --srpm --qf '%{version}' "$SPEC")
release=$(rpmspec -q --srpm --define "dist $DIST" --qf '%{release}' "$SPEC")
echo "building ${name}-${version}-${release} from $(git describe --tags --always)"

top=$(rpm --eval '%{_topdir}')
mkdir -p "$top/SOURCES"
git archive --prefix="${name}-${version}/" HEAD | gzip -7 > "$top/SOURCES/${name}-${version}.tar.gz"
dnf -y -q builddep --define "dist $DIST" "$SPEC"
rpmbuild -ba --define "dist $DIST" "$SPEC"

# the installed xrootd must still be the one we asked for (builddep must not have upgraded it)
rpm -q --qf '%{epoch}:%{version}-%{release}\n' xrootd-server-devel | grep -qx "1:${XROOTD_VERSION}" \
  || { echo "xrootd-server-devel is $(rpm -q xrootd-server-devel), expected ${XROOTD_VERSION}" >&2; exit 1; }

mkdir -p "$OUT"
cp -v "$top"/RPMS/*/"${name}"-*.rpm "$top"/SRPMS/"${name}-${version}-${release}".src.rpm "$OUT"/
rm -f "$OUT"/*-debuginfo-*.rpm "$OUT"/*-debugsource-*.rpm
ls -l "$OUT"
