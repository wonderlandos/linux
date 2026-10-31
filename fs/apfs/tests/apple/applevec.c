#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apfs.h"
#include "compress_codec.h"
/* Decode every block in a file of records (algo, want, clen, cdata) exported from a real Apple volume.
 * Checks the decoder reports exactly `want` bytes for each. Apple content stays outside the repository. */
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	static unsigned char c[1 << 20], out[1 << 16];
	unsigned h[3];
	long ok = 0, bad = 0, byalgo[16] = {0};

	while (f && fread(h, 4, 3, f) == 3) {
		size_t ol = 0;
		int r;

		if (h[2] > sizeof c || fread(c, 1, h[2], f) != h[2]) { puts("bad record"); return 2; }
		r = apfs_decompress_block(h[0], c, h[2], out, sizeof out, h[1], &ol);
		if (!r && ol == h[1]) { ok++; byalgo[h[0] & 15]++; }
		else { if (bad++ < 5) printf("MISMATCH algo %u want %u got %zu rc %d clen %u\n", h[0], h[1], ol, r, h[2]); }
	}
	printf("apple blocks decoded to the expected size: %ld ok, %ld bad (algo 7:%ld 8:%ld 9:%ld 11:%ld)\n", ok, bad,
	       byalgo[7], byalgo[8], byalgo[9], byalgo[11]);
	return bad != 0;
}
