"""Resource-fork layout facts from real Apple data (uses apfsdump.py's parser): rsrc.py image.raw
Reads the com.apple.ResourceFork stream of lzvn-rsrc / zlib-rsrc / lzfse-rsrc files and prints the structure
of their header, block table and trailer, and how much of it is constant across files."""
import sys, struct, collections, hashlib
OUT = open(sys.argv[2], 'wb') if len(sys.argv) > 2 else None
sys.argv = [sys.argv[0], sys.argv[1]]
src = open(__file__.replace('rsrc.py', 'apfsdump.py')).read().split("ALG = {")[0]
exec(src)
extents_by = collections.defaultdict(list)

# second pass over the catalog for extent records (apfsdump keeps only counts)
def walk_ext(oid):
    b = blk(oid if PHYS else vmap[oid][1])
    flags, level, nkeys, _, _ = node(b)
    for k, v in entries(b):
        if level > 0:
            c = struct.unpack('<Q', v[:8])[0]; walk_ext(c + root_oid if flags & 8 else c); continue
        if len(k) < 16: continue
        h = struct.unpack('<Q', k[:8])[0]
        if h >> 60 == 8:
            la = struct.unpack_from('<Q', k, 8)[0]
            lf, pb, cid = struct.unpack_from('<QQQ', v, 0)
            extents_by[h & ((1 << 60) - 1)].append((la, lf & 0xffffffffffffff, pb))
walk_ext(root_oid)
fext_oid = u('Q', vsb, 0x408)[0]; fext_type = u('I', vsb, 0x410)[0]
print('fext tree oid', fext_oid, 'type %#x' % fext_type)
def walk_fext(oid):
    b = blk(oid if fext_type & 0x40000000 else vmap[oid][1])
    flags, level, nkeys, _, _ = node(b)
    for k, v in entries(b, (16, 16)):
        if level > 0:
            walk_fext(struct.unpack('<Q', v[:8])[0]); continue
        pid, la = struct.unpack('<QQ', k)
        lf, pb = struct.unpack('<QQ', v)
        extents_by[pid].append((la, lf & 0xffffffffffffff, pb))
if fext_oid:
    walk_fext(fext_oid)
print('extent owners', len(extents_by))

def read_stream(xv):
    fl, ln = struct.unpack_from('<HH', xv, 0)
    if fl & 2:
        return xv[4:4 + ln]
    xid_obj, size = struct.unpack_from('<QQ', xv, 4)
    out = bytearray()
    for la, length, pb in sorted(extents_by[xid_obj]):
        out += blk_range(pb, length)
    return bytes(out[:size])

def blk_range(pb, length):
    img.seek(pb * BS); return img.read(length)

stats = collections.Counter(); ex = []
for ino, v in inodes.items():
    if not struct.unpack_from('<I', v, 68)[0] & 0x20: continue
    xa = xattrs.get(ino, {})
    dec = xa.get('com.apple.decmpfs'); rs = xa.get('com.apple.ResourceFork')
    if dec is None or rs is None: continue
    sig, algo, size = struct.unpack_from('<IIQ', dec, 4)
    data = read_stream(rs)
    first = struct.unpack_from('<I', data, 0)[0] if len(data) >= 4 else 0
    n = first // 4 - 1
    ok = first % 4 == 0 and 1 <= n <= 100000 and len(data) >= first
    if not ok:
        stats[('bad table head', algo)] += 1; continue
    offs = struct.unpack_from('<%dI' % (n + 1), data, 0)
    mono = all(offs[i] < offs[i + 1] for i in range(n))
    want_n = (size + 65535) // 65536
    stats[('algo', algo, 'blocks==ceil(size/64K)', n == want_n, 'monotonic', mono, 'last==len(data)', offs[-1] == len(data),
           'extra after last', len(data) - offs[-1] if offs[-1] <= len(data) else 'overrun')] += 1
    if OUT:
        for bi in range(n):
            blkd = data[offs[bi]:offs[bi + 1]]
            OUT.write(struct.pack('<III', algo, min(65536, size - bi * 65536), len(blkd)) + blkd)
    first_bytes = collections.Counter(data[offs[i]] for i in range(n) if offs[i] < len(data))
    stats[('first byte of blocks', tuple(sorted(first_bytes)))] += 1
    if len(ex) < 3 or (n > 1 and len(ex) < 5):
        ex.append((ino, size, len(data), n, offs[:4], offs[-1], data[offs[0]:offs[0] + 8].hex(), data[-8:].hex()))
for k, c in sorted(stats.items(), key=lambda x: -x[1])[:14]: print(c, k)
for e in ex: print(e)
# inline (attr) forms: the block is what follows the 16-byte decmpfs header
if OUT:
    na = 0
    for ino, v in inodes.items():
        if not struct.unpack_from('<I', v, 68)[0] & 0x20: continue
        dec = xattrs.get(ino, {}).get('com.apple.decmpfs')
        if dec is None: continue
        fl, ln = struct.unpack_from('<HH', dec, 0)
        sig, algo, size = struct.unpack_from('<IIQ', dec, 4)
        if algo in (3, 7, 9, 11, 13) and fl & 2:
            blkd = dec[20:4 + ln]
            OUT.write(struct.pack('<III', algo, size, len(blkd)) + blkd); na += 1
    print('inline blocks exported', na)
    OUT.close()