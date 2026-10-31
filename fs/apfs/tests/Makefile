# Userspace tests for the decmpfs block decoders (compress_codec.c and the LZFSE, LZVN and
# zbitmap decoders it calls), built with ASan and UBSan against small kernel-header shims.
# Vectors: raw* are inputs, seed_f* come from Apple's lzfse tool (github.com/lzfse/lzfse),
# seed_n* from its LZVN encoder, seed_z* from zlib.
#   make check          decode every vector and compare with the original
#   make fuzz N=500000  mutate the vectors and decode; any sanitizer report fails
D := ..
# The sources are copied next to the shims: the real apfs.h pulls in half the kernel.
B := build
SRCS := compress_codec.c libzbitmap.c lzfse/lzfse_decode.c lzfse/lzfse_decode_base.c \
        lzfse/lzfse_fse.c lzfse/lzvn_decode_base.c
HDRS := compress_codec.h libzbitmap.h $(wildcard $(D)/lzfse/*.h)
SRC := $(addprefix $(B)/,$(SRCS)) $(B)/zshim.c
CF := -g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$(B) -I$(B)/lzfse
N ?= 200000
$(B)/stamp: $(wildcard $(D)/*.c $(D)/*.h $(D)/lzfse/*)
	rm -rf $(B); mkdir -p $(B)/lzfse
	cp -r shim/linux shim/apfs.h shim/zshim.c $(B)/
	for f in $(SRCS) compress_codec.h libzbitmap.h; do cp $(D)/$$f $(B)/$$f; done
	cp $(D)/lzfse/*.h $(B)/lzfse/
	touch $@
SEEDS := $(foreach i,0 1 4 5,3 vectors/seed_z$(i) 11 vectors/seed_f$(i) 7 vectors/seed_n$(i))
vec: vec.c $(B)/stamp
	$(CC) $(CF) -o $@ vec.c $(SRC) -lz
fuzz: fuzz.c $(B)/stamp
	$(CC) $(CF) -o $@ fuzz.c $(SRC) -lz
check: vec
	cd vectors && ../vec
fuzz-run: fuzz
	./fuzz $(N) 1 $(SEEDS)
clean:
	rm -rf vec fuzz build
.PHONY: check fuzz-run clean
