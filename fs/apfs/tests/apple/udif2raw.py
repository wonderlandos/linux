"""Rebuild the raw partition image from an Apple UDIF .dmg (koly trailer): udif2raw.py in.dmg out.raw [partition-name-substring]
Handles zero, copy, zlib and bzip2 runs (what the system images use); reports anything else."""
import sys, struct, plistlib, zlib, bz2

inp, outp = sys.argv[1], sys.argv[2]
want = sys.argv[3] if len(sys.argv) > 3 else 'Apple_APFS'
f = open(inp, 'rb')
f.seek(-512, 2)
k = f.read(512)
assert k[:4] == b'koly', 'no koly trailer'
xml_off, xml_len = struct.unpack('>QQ', k[0xd8:0xe8])
f.seek(xml_off)
pl = plistlib.loads(f.read(xml_len))
parts = pl['resource-fork']['blkx']
for p in parts:
    print('partition', p['Name'], p['ID'])
sel = [p for p in parts if want in p['Name']] or parts
p = max(sel, key=lambda p: len(p['Data']))
d = p['Data']
assert d[:4] == b'mish'
first_sector, n_sectors, data_off = struct.unpack('>QQQ', d[8:32])
n_runs = struct.unpack('>I', d[200:204])[0]
out = open(outp, 'wb')
out.truncate(n_sectors * 512)
kinds = {}
for i in range(n_runs):
    r = d[204 + 40 * i: 244 + 40 * i]
    t, _, s_off, s_cnt, c_off, c_len = struct.unpack('>IIQQQQ', r)
    kinds[t] = kinds.get(t, 0) + 1
    if t == 0xffffffff:
        break
    out.seek(s_off * 512)
    n = s_cnt * 512
    if t in (0, 2):
        continue  # zero / ignore: the sparse file is already zero
    f.seek(data_off + c_off)
    raw = f.read(c_len)
    if t == 1:
        out.write(raw)
    elif t == 0x80000005:
        out.write(zlib.decompress(raw))
    elif t == 0x80000006:
        out.write(bz2.decompress(raw))
    elif t == 0x7ffffffe:
        pass
    else:
        sys.exit('unsupported run type %#x' % t)
print('run kinds', {hex(a): b for a, b in kinds.items()}, 'sectors', n_sectors)
