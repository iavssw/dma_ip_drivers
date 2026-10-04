/* Parser-only regression: no device files, netlink or register accesses. */
#include <assert.h>
#include <stdio.h>
#include "cmd_parse.h"

int main(void)
{
	struct xcmd_info cmd;
	char *args[] = {"dma-ctl", "qdma11000", "q", "start", "idx", "1",
		"dir", "bi", "mm_chn", "1", "mm_host_id", "0"};
	char value[16];
	const char *bad[] = {"16", "-1", "256", "invalid"};
	unsigned int i;

	for (i = 0; i <= 15; ++i) {
		snprintf(value, sizeof(value), "%u", i);
		args[11] = value;
		assert(parse_cmd(12, args, &cmd) == 0);
		assert(cmd.req.qparm.mm_channel == 1);
		assert(cmd.req.qparm.mm_hostid == i);
		assert(cmd.req.qparm.sflags & (1U << QPARM_MM_HOST_ID));
	}
	/* The two fields are deliberately independent. */
	args[9] = "0";
	args[11] = "1";
	assert(parse_cmd(12, args, &cmd) == 0);
	assert(cmd.req.qparm.mm_channel == 0);
	assert(cmd.req.qparm.mm_hostid == 1);
	args[9] = "1";
	assert(parse_cmd(10, args, &cmd) == 0);
	assert(!(cmd.req.qparm.sflags & (1U << QPARM_MM_HOST_ID)));
	assert(cmd.req.qparm.mm_hostid == 0);
	assert(parse_cmd(11, args, &cmd) < 0);
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
		args[11] = (char *)bad[i];
		assert(parse_cmd(12, args, &cmd) < 0);
	}
	puts("PASS: mm_host_id parser values, independent channel, default and errors");
	return 0;
}
