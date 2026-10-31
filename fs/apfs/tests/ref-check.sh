#!/bin/sh
# Decode the streams written by `make check` (encout/) with Apple's reference code.
# Usage: LZFSE_REF=/path/to/lzfse-checkout ./ref-check.sh   (build it first with make)
# Types 11 are fed to the reference lzfse tool, type 7 to its LZVN decoder, type 3 to zlib.
set -e
R=${LZFSE_REF:?set LZFSE_REF to a checkout of github.com/lzfse/lzfse}
cd "$(dirname "$0")/encout"
cat > /tmp/lvnref.c <<'E'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lzvn_decode_base.h"
int main(int c, char **v) {
	static unsigned char in[70000], out[70000], want[70000];
	FILE *f = fopen(v[1], "rb"); size_t n = fread(in, 1, sizeof in, f); fclose(f);
	f = fopen(v[2], "rb"); size_t w = fread(want, 1, sizeof want, f); fclose(f);
	lzvn_decoder_state s; memset(&s, 0, sizeof s);
	s.src = in; s.src_end = in + n; s.dst = s.dst_begin = out; s.dst_end = out + sizeof out;
	lzvn_decode(&s);
	return !((size_t)(s.dst - out) == w && !memcmp(out, want, w));
}
E
cc -O1 -I"$R/src" -o /tmp/lvnref /tmp/lvnref.c "$R/src/lzvn_decode_base.c"
ok=0; bad=0
for o in enc.out.*; do
	i=enc.in.${o#enc.out.}; t=${o#enc.out.}; t=${t%%.*}
	case $t in
	11) "$R/build/bin/lzfse" -decode -i "$o" -o /tmp/ref.dec && cmp -s /tmp/ref.dec "$i" ;;
	7)  /tmp/lvnref "$o" "$i" ;;
	3)  python3 -c 'import sys,zlib; sys.exit(zlib.decompress(open(sys.argv[1],"rb").read())!=open(sys.argv[2],"rb").read())' "$o" "$i" ;;
	esac && ok=$((ok+1)) || { echo "reference decode FAILED: $o"; bad=$((bad+1)); }
done
echo "reference decoders: $ok streams ok, $bad failed"
[ $bad -eq 0 ]
