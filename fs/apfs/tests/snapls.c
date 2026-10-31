#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <unistd.h>
/* Lists the snapshots of a mounted APFS volume: snapls MOUNTPOINT */
#define APFS_SNAP_MAX_NAMELEN 255
#define APFS_SNAP_LIST_MAX 32
struct apfs_ioctl_snap_list {
	__u32 count, total;
	struct { __u64 xid; char name[APFS_SNAP_MAX_NAMELEN + 1]; } snap[APFS_SNAP_LIST_MAX];
};
#define APFS_IOC_LIST_SNAPSHOTS _IOR('@', 0x86, struct apfs_ioctl_snap_list)
int main(int argc, char **argv)
{
	struct apfs_ioctl_snap_list *l = calloc(1, sizeof(*l));
	int fd = open(argc > 1 ? argv[1] : ".", O_RDONLY | O_DIRECTORY);

	if (fd < 0 || ioctl(fd, APFS_IOC_LIST_SNAPSHOTS, l)) { perror("snapls"); return 1; }
	for (unsigned i = 0; i < l->count; i++)
		printf("%llu\t%s\n", (unsigned long long)l->snap[i].xid, l->snap[i].name);
	if (l->total > l->count)
		printf("(%u more)\n", l->total - l->count);
	return 0;
}
