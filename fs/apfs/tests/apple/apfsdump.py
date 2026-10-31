#!/usr/bin/env python3
"""Read-only APFS inspector written from Apple's public File System Reference, independent of the
kernel module: lists compressed inodes (bsd flag 0x20) with their decmpfs header, resource-fork
xattr, inode flags and xfields. Usage: apfsdump.py image.raw [--volume N] [--list N] [--tree PATH-ID]"""
import sys, struct, collections

img = open(sys.argv[1], 'rb')
BS = 4096

def blk(n):
    img.seek(n * BS); return img.read(BS)

def u(fmt, b, off):
    return struct.unpack_from('<' + fmt, b, off)

# container superblock: pick the newest valid checkpoint copy
nx = blk(0)
assert nx[32:36] == b'NXSB', 'not an APFS container'
BS = u('I', nx, 36)[0]
desc_blocks = u('I', nx, 104)[0] & 0x7fffffff
desc_base = u('Q', nx, 112)[0]
best = nx; best_xid = u('Q', nx, 16)[0]
for i in range(desc_blocks):
    b = blk(desc_base + i)
    if b[32:36] == b'NXSB' and u('Q', b, 16)[0] > best_xid:
        best, best_xid = b, u('Q', b, 16)[0]
nx = best
omap_oid = u('Q', nx, 160)[0]
fs_oids = [x for x in struct.unpack_from('<100Q', nx, 184) if x]

def node(b):
    flags, level, nkeys = u('HHI', b, 32)
    toc_off, toc_len = u('HH', b, 40)
    return flags, level, nkeys, toc_off, toc_len

def entries(b, key_size_fixed=None):
    """yield (key bytes, value bytes) of a node"""
    flags, level, nkeys, toc_off, toc_len = node(b)
    keys0 = 56 + toc_off + toc_len
    end = BS - (40 if flags & 1 else 0)
    toc = 56 + toc_off
    for i in range(nkeys):
        if flags & 4:
            ko, vo = u('HH', b, toc + 4 * i)
            kl = key_size_fixed[0]; vl = key_size_fixed[1] if level == 0 else 8
        else:
            ko, kl, vo, vl = u('HHHH', b, toc + 8 * i)
        k = b[keys0 + ko: keys0 + ko + kl]
        v = b[end - vo: end - vo + vl] if vo != 0xffff else b''
        yield k, v

def omap_map(omap_phys_blk):
    ob = blk(omap_phys_blk)
    tree = u('Q', ob, 48)[0]
    m = {}
    def walk(paddr):
        b = blk(paddr)
        flags, level, nkeys, _, _ = node(b)
        for k, v in entries(b, (16, 16)):
            if level == 0:
                oid, xid = struct.unpack('<QQ', k[:16])
                paddr2 = struct.unpack_from('<Q', v, 8)[0]
                if oid not in m or m[oid][0] < xid:
                    m[oid] = (xid, paddr2)
            else:
                walk(struct.unpack('<Q', v[:8])[0])
    walk(tree)
    return m

cmap = omap_map(omap_oid)
vol_idx = 0
if '--volume' in sys.argv:
    vol_idx = int(sys.argv[sys.argv.index('--volume') + 1])
vsb = blk(cmap[fs_oids[vol_idx]][1])
assert vsb[32:36] == b'APSB'
incompat = u('Q', vsb, 56)[0]
vname = vsb[704:704 + 256].split(b'\0')[0].decode(errors='replace')
vomap_oid = u('Q', vsb, 128)[0]
root_oid = u('Q', vsb, 136)[0]
root_type = u('I', vsb, 116)[0]
PHYS = bool(root_type & 0x40000000)
print('container block size', BS, 'xid', best_xid, 'volumes', len(fs_oids))
print('catalog root type %#x' % root_type, 'physical' if PHYS else 'virtual')
print('volume', vol_idx, repr(vname), 'incompat features %#x' % incompat,
      '(sealed)' if incompat & 0x20 else '', '(case-insens.)' if incompat & 1 else '')
vmap = omap_map(vomap_oid)

inodes, xattrs, extents = {}, collections.defaultdict(dict), collections.defaultdict(int)
def walk_cat(oid):
    paddr = oid if PHYS else vmap[oid][1]
    b = blk(paddr)
    flags, level, nkeys, _, _ = node(b)
    for k, v in entries(b):
        if level > 0:
            c = struct.unpack('<Q', v[:8])[0]
            walk_cat(c + root_oid if flags & 8 else c); continue
        if len(k) < 8: continue
        h = struct.unpack('<Q', k[:8])[0]
        typ, id_ = h >> 60, h & ((1 << 60) - 1)
        if typ == 3:
            inodes[id_] = v
        elif typ == 4:
            nl = struct.unpack_from('<H', k, 8)[0]
            xattrs[id_][k[10:10 + nl].rstrip(b'\0').decode(errors='replace')] = v
        elif typ == 8:
            extents[id_] += 1
walk_cat(root_oid)

def xfields(v):
    out = {}
    if len(v) <= 92 + 4: return out
    num, used = struct.unpack_from('<HH', v, 92)
    pos = 92 + 4 + 4 * num
    for i in range(num):
        t, f, s = struct.unpack_from('<BBH', v, 96 + 4 * i)
        out[t] = (f, v[pos:pos + s]); pos += (s + 7) & ~7
    return out

ALG = {3: 'zlib-attr', 4: 'zlib-rsrc', 7: 'lzvn-attr', 8: 'lzvn-rsrc', 9: 'plain-attr', 10: 'plain-rsrc',
       11: 'lzfse-attr', 12: 'lzfse-rsrc', 13: 'lzbitmap-attr', 14: 'lzbitmap-rsrc'}
hist = collections.Counter(); examples = {}
for ino, v in inodes.items():
    bsd = struct.unpack_from('<I', v, 68)[0]
    if not bsd & 0x20: continue
    intf = struct.unpack_from('<Q', v, 48)[0]
    unc = struct.unpack_from('<Q', v, 84)[0]
    xf = xfields(v)
    xa = xattrs.get(ino, {})
    dec = xa.get('com.apple.decmpfs')
    if dec is None:
        hist[('no decmpfs xattr',)] += 1; continue
    fl, ln = struct.unpack_from('<HH', dec, 0)
    data = dec[4:4 + ln] if fl & 2 else b''
    sig, algo, size = struct.unpack_from('<IIQ', data, 0) if len(data) >= 16 else (0, 0, 0)
    key = (ALG.get(algo, algo), 'sig=%08x' % sig, 'xattr-flags=%#x' % fl, 'dstream-xfield=%s' % (8 in xf),
           'has-rsrc-xattr=%s' % ('com.apple.ResourceFork' in xa),
           'int-flags-bits=%#x' % (intf & 0x4000 | intf & 0x8000 | intf & 0x40000),
           'unc-size-set=%s' % (unc != 0), 'unc==size=%s' % (unc == size), 'extents=%s' % bool(extents.get(ino)))
    hist[key] += 1
    examples.setdefault(key, (ino, len(data), data[:48].hex()))
print('compressed inodes:', sum(hist.values()), 'of', len(inodes), 'inodes')
for key, n in hist.most_common():
    print('%6d  %s' % (n, ' '.join(map(str, key))))
    if key in examples: print('        e.g. ino %s decmpfs len %s head %s' % examples[key])
