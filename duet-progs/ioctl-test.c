#include <stdio.h>
#include <stdlib.h>
#include "ioctl.h"

static unsigned long ioctls[] = {
	DUET_IOC_CMD,
	DUET_IOC_TLIST,
	DUET_IOC_FETCH,
	0 };

int main(int ac, char **av)
{
	int i = 0;
	while(ioctls[i]) {
		printf("%lu\n", ioctls[i]);
		i++;
	}
	return 0;
}

