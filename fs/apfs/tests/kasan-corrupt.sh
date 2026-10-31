#!/bin/bash
# Corrupt the compressed streams inside a compressed-image and read every file back, under KASAN.
# usage: kasan-corrupt.sh image iterations   (root, a KASAN kernel; image from compress-stress.sh)
src=$1; N=${2:-100}; mp=/mnt/kf; img=/home/arch/kf.img
mkdir -p $mp; dmesg -c >/dev/null
for i in $(seq 1 $N); do
  cp $src $img
  python3 - $img $i <<'E'
import sys, random, mmap
p, seed = sys.argv[1], int(sys.argv[2]); random.seed(seed)
f = open(p, 'r+b'); m = mmap.mmap(f.fileno(), 0)
offs = []; i = m.find(b'fpmc')
while i >= 0 and len(offs) < 4000:
    offs.append(i); i = m.find(b'fpmc', i + 1)
for _ in range(random.randint(1, 12)):
    o = random.choice(offs); hdr = random.random() < 0.3
    at = o + (random.randint(4, 15) if hdr else random.randint(16, 140))
    m[at] = random.randrange(256)
m.flush(); m.close(); f.close()
E
  mount -t apfs -o loop,ro $img $mp 2>/dev/null || { umount $mp 2>/dev/null; continue; }
  find $mp -type f -exec cat {} + > /dev/null 2>&1
  umount $mp
done
echo "corrupt-image rounds: $N"
echo "kasan/ubsan/oops lines: $(dmesg | grep -ciE 'kasan|ubsan|call trace|bug:|general protection|oops')"
dmesg | grep -iE 'kasan|ubsan|call trace|bug:|general protection|oops' | head -10
echo "decompression errors logged (expected): $(dmesg | grep -ciE 'decompression failed|bad compressed|failed to read')"
