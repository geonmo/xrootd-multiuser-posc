#!/bin/bash
# Check that the built plugins link cleanly and load in a real xrootd server,
# then upload a file through them and read it back.
#   smoke-test.sh <xrootd prefix> <libXrdMultiuser-N.so> [<libXrdMultiuserPosc-N.so>]
# Must run as root: xrootd drops to "nobody" with -R, as multiuser needs.
set -euo pipefail
prefix=$1
mu_so=$2
posc_so=${3:-}
port=21094

export PATH="$prefix/bin:$PATH"
export LD_LIBRARY_PATH="$prefix/lib64:$prefix/lib"

echo "::group::Unresolved symbols"
for so in "$mu_so" $posc_so; do
  echo "$so"
  if ldd -r "$so" 2>&1 | grep -E 'undefined symbol|not found'; then
    echo "::error::$so has unresolved symbols"; exit 1
  fi
done
echo "::endgroup::"

rt=$(mktemp -d /tmp/xrd-smoke.XXXXXX)
chmod 755 "$rt"
mkdir -p "$rt/lib" "$rt/data" "$rt/admin" "$rt/journal"
chown nobody "$rt/data" "$rt/admin" "$rt/journal"
chmod 1733 "$rt/journal"
cp "$mu_so" $posc_so "$rt/lib/"
# The config names the plugins without the -<major> suffix; xrootd adds it.
unversioned() { basename "$1" | sed -E 's/-[0-9]+\.so$/.so/'; }

{
  echo "all.export /"
  echo "oss.localroot $rt/data"
  echo "xrd.port $port"
  echo "xrd.adminpath $rt/admin"
  echo "all.pidpath $rt"
  [ -n "$posc_so" ] && echo "ofs.osslib ++ $rt/lib/$(unversioned "$posc_so")"
  echo "ofs.osslib ++ $rt/lib/$(unversioned "$mu_so")"
  echo "ofs.ckslib * $rt/lib/$(unversioned "$mu_so")"
  echo "xrootd.chksum md5 adler32 crc32"
  if [ -n "$posc_so" ]; then
    echo "multiuser.posc.enable on"
    echo "multiuser.posc.journal $rt/journal"
  fi
} > "$rt/xrootd.cfg"

xrootd -R nobody -c "$rt/xrootd.cfg" -n smoke > "$rt/xrootd.log" 2>&1 &
xrd_pid=$!
trap 'kill $xrd_pid 2>/dev/null || true; echo "::group::xrootd log"; cat "$rt/xrootd.log"; echo "::endgroup::"' EXIT

for _ in $(seq 60); do
  grep -q 'initialization completed' "$rt/xrootd.log" && break
  if grep -q 'initialization failed' "$rt/xrootd.log" || ! kill -0 $xrd_pid 2>/dev/null; then
    echo "::error::xrootd failed to start"; exit 1
  fi
  sleep 1
done
grep -q 'initialization completed' "$rt/xrootd.log" || { echo "::error::xrootd did not start in time"; exit 1; }

grep -q 'Plugin loaded osg-multiuser' "$rt/xrootd.log" || { echo "::error::multiuser plugin not loaded"; exit 1; }
if [ -n "$posc_so" ]; then
  grep -q 'Plugin loaded XrdMultiuserPosc' "$rt/xrootd.log" || { echo "::error::posc plugin not loaded"; exit 1; }
fi

url=root://127.0.0.1:$port
head -c 1048576 /dev/urandom > "$rt/in.bin"
xrdcp --nopbar "$rt/in.bin" "$url//smoke.bin"
xrdcp --nopbar "$url//smoke.bin" "$rt/out.bin"
cmp "$rt/in.bin" "$rt/out.bin"

remote=$(xrdfs 127.0.0.1:$port query checksum /smoke.bin | awk '{print $2}')
local_sum=$(md5sum "$rt/in.bin" | awk '{print $1}')
[ "$remote" = "$local_sum" ] || { echo "::error::checksum mismatch: $remote != $local_sum"; exit 1; }

owner=$(stat -c %U "$rt/data/smoke.bin")
[ "$owner" = nobody ] || { echo "::error::smoke.bin owned by $owner, expected nobody"; exit 1; }

if [ -n "$posc_so" ] && ls -A "$rt/data" | grep -q '\.mu-part\.'; then
  echo "::error::POSC temporary file left behind"; ls -la "$rt/data"; exit 1
fi

echo "Smoke test passed: upload, download, md5 checksum, owner=$owner"
