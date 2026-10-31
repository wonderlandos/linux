#!/bin/bash
# Stress for chattr +c on a scratch image (root): APFSPROGS=/path ./compress-stress.sh <zlib|lzvn|lzfse> [nfiles]
# Compresses, checks md5 on the same mount and after remount, runs apfsck, then removes, renames and links
# compressed files and checks again. Needs the apfs module built with compression.
A=$1; N=${2:-300}
P=${APFSPROGS:?set APFSPROGS to a built apfsprogs checkout}; W=${WORK:-/var/tmp}; img=$W/cm_$A.img; mp=/mnt/cm_$A; man=$W/cm_$A.md5
mkdir -p $mp; rm -f $img; truncate -s 1G $img; $P/mkapfs/mkapfs -L cm $img >/dev/null
$P/apfsck/apfsck $img; echo "[$A] fresh apfsck rc=$?"
dmesg -c >/dev/null
mount -t apfs -o loop,readwrite,compress=$A $img $mp || exit 1
python3 - $mp $N <<'E'
import os, random, sys
mp, n = sys.argv[1], int(sys.argv[2]); random.seed(5)
words = [b"hello ", b"apfs ", b"decmpfs ", b"WolOS ", b"\x00\x00\x00\x00", b"lorem ipsum dolor ", b"0123456789"]
for i in range(n):
    sz = random.choice([1, 5, 40, 300, 1500, 3000, 4096, 5000, 9000, 20000, 40000, 65536, 70000])
    sz = max(1, sz + random.randint(-3, 3))
    kind = random.choice(["text", "text", "text", "zero", "rand"])
    if kind == "text": d = b"".join(random.choice(words) for _ in range(sz // 4 + 1))[:sz]
    elif kind == "zero": d = bytes(sz)
    else: d = os.urandom(sz)
    open("%s/f%04d" % (mp, i), "wb").write(d)
E
sync
(cd $mp; md5sum f* > $man)
ok=0; nospc=0; unsup=0; other=0
for f in $mp/f*; do
  e=$(chattr +c $f 2>&1); rc=$?
  if [ $rc -eq 0 ]; then ok=$((ok+1)); elif echo "$e" | grep -q "No space"; then nospc=$((nospc+1)); elif echo "$e" | grep -q "not supported"; then unsup=$((unsup+1)); else other=$((other+1)); echo "other: $e"; fi
done
echo "[$A] chattr +c: compressed=$ok refused-no-shrink=$nospc too-big=$unsup other=$other"
(cd $mp; md5sum -c $man 2>&1 | grep -vc ": OK$" | sed "s/^/[$A] md5 mismatches same mount: /")
umount $mp; echo "[$A] umount rc=$?"
$P/apfsck/apfsck $img; echo "[$A] apfsck after compress rc=$?"
mount -t apfs -o loop,ro $img $mp; (cd $mp; md5sum -c $man 2>&1 | grep -vc ": OK$" | sed "s/^/[$A] md5 mismatches after remount ro: /"; echo "[$A] compressed-flag files: $(lsattr f* 2>/dev/null | grep -c -- '-c-')"); umount $mp
mount -t apfs -o loop,readwrite,compress=$A $img $mp
# delete, rename, link some compressed files
(cd $mp; i=0; for f in f*; do i=$((i+1)); case $((i%5)) in 0) rm -f $f; sed -i "/ $f\$/d" $man;; 1) mv $f g$f; sed -i "s/ $f\$/ g$f/" $man;; 2) ln $f h$f; echo "$(md5sum < $f | cut -d' ' -f1)  h$f" >> $man;; esac; done)
sync; (cd $mp; md5sum -c $man 2>&1 | grep -vc ": OK$" | sed "s/^/[$A] md5 mismatches after rm\/mv\/ln: /")
umount $mp; $P/apfsck/apfsck $img; echo "[$A] apfsck after rm/mv/ln rc=$?"
mount -t apfs -o loop,ro $img $mp; (cd $mp; md5sum -c $man 2>&1 | grep -vc ": OK$" | sed "s/^/[$A] md5 mismatches final: /"); umount $mp
echo "[$A] dmesg bad lines: $(dmesg | grep -ciE 'warn|bug|oops|error|call trace|kasan|ubsan')"
dmesg | grep -iE 'warn|bug|oops|error|call trace|kasan|ubsan' | head -5
echo "[$A] DONE"
