/*
 * ccstart: CommCenter's launchd program on sapphire. launchd drops DYLD_* variables for system
 * daemons, so this sets DYLD_INSERT_LIBRARIES=/redwood/lib/ccbridge.dylib and execs the real
 * CommCenter (same pid: launchd's job, its Mach services and check-ins stay as they are).
 * Mobile data (qmitool autodata) is started once by ccbridge_init() after unsetting DYLD_INSERT_LIBRARIES.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define COMMCENTER "/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter"
#define CCBRIDGE   "/redwood/lib/ccbridge.dylib"

int
main(int argc, char **argv)
{
	(void)argc;
	if (access(CCBRIDGE, R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", CCBRIDGE, 1);
	}
	argv[0] = COMMCENTER;
	execv(COMMCENTER, argv);
	perror("ccstart: exec CommCenter");
	return 1;
}
