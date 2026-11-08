#!/bin/bash
# Build the RPM and SRPM inside an AlmaLinux 9 container (GitHub Actions).
#   SPEC            spec file path relative to the repository root
#   DIST            %{dist} value, e.g. .osg25.el9
#   XROOTD_MAJOR    5 or 6 (passed to the spec as xrootd_major: dependency range, plugin suffix)
#   XROOTD_VERSION  xrootd devel packages to build against (epoch 1), e.g. 5.9.7-1.2.osg25.el9
#   XROOTD_REPOS    extra OSG repositories to enable, e.g. osg-upcoming-development (optional)
#   OUT             directory to copy the built RPMs into (default: dist/)
# KISTI-GSDC runs xrootd 5.9.7-1.2.1 (OSG 1.2 + a .cc-only patch, same headers), so the
# xrootd 5 build uses the OSG 1.2 devel packages.
# Usage: rpm/ci-build.sh   (run from the repository root, checkout of the commit to build)
set -euo pipefail
: "${SPEC:?}" "${DIST:?}" "${XROOTD_MAJOR:?}" "${XROOTD_VERSION:?}"
OUT=${OUT:-dist}
enable=()
for r in ${XROOTD_REPOS:-}; do enable+=(--enablerepo="$r"); done

dnf -y -q install epel-release dnf-plugins-core
dnf config-manager --set-enabled crb
dnf -y -q install https://repo.opensciencegrid.org/osg/25-main/osg-25-main-el9-release-latest.rpm
if ! dnf -y -q "${enable[@]}" install git rpm-build rpmdevtools gzip \
  "xrootd-devel-1:${XROOTD_VERSION}" "xrootd-server-devel-1:${XROOTD_VERSION}" \
  "xrootd-private-devel-1:${XROOTD_VERSION}" "xrootd-server-libs-1:${XROOTD_VERSION}"; then
  # OSG moves builds between repositories (development -> testing -> release); show
  # where xrootd is now so XROOTD_VERSION / XROOTD_REPOS can be fixed
  echo "xrootd ${XROOTD_VERSION} not found with repos: ${XROOTD_REPOS:-<default>}" >&2
  dnf -q --enablerepo='osg*' --showduplicates repoquery --qf '%{repoid} %{name}-%{epoch}:%{version}-%{release}' \
    xrootd-server-devel >&2 || true
  exit 1
fi
git config --global --add safe.directory "$PWD"

defs=(--define "dist $DIST" --define "xrootd_major $XROOTD_MAJOR")
name=$(rpmspec -q --srpm "${defs[@]}" --qf '%{name}' "$SPEC")
version=$(rpmspec -q --srpm "${defs[@]}" --qf '%{version}' "$SPEC")
release=$(rpmspec -q --srpm "${defs[@]}" --qf '%{release}' "$SPEC")
echo "building ${name}-${version}-${release} (xrootd ${XROOTD_VERSION}) from $(git describe --tags --always)"

top=$(rpm --eval '%{_topdir}')
mkdir -p "$top/SOURCES"
git archive --prefix="${name}-${version}/" HEAD | gzip -7 > "$top/SOURCES/${name}-${version}.tar.gz"
dnf -y -q "${enable[@]}" builddep "${defs[@]}" "$SPEC"
rpmbuild -ba "${defs[@]}" "$SPEC"

# the installed xrootd must still be the one we asked for (builddep must not have changed it)
rpm -q --qf '%{epoch}:%{version}-%{release}\n' xrootd-server-devel | grep -qx "1:${XROOTD_VERSION}" \
  || { echo "xrootd-server-devel is $(rpm -q xrootd-server-devel), expected ${XROOTD_VERSION}" >&2; exit 1; }

mkdir -p "$OUT"
cp -v "$top"/RPMS/*/"${name}"-*.rpm "$top"/SRPMS/"${name}-${version}-${release}".src.rpm "$OUT"/
rm -f "$OUT"/*-debuginfo-*.rpm "$OUT"/*-debugsource-*.rpm
ls -l "$OUT"
