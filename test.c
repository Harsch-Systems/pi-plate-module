#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

#include "piplate.h"

int main() {
	FILE *fp;
	fp = fopen("/dev/PiPlates", "r");
	if(fp == NULL){
		printf("error!");
		exit(1);
	}

	unsigned char val = 53;
	ioctl(fileno(fp), PIPLATE_GETADDR, &val);

	printf("%d\n", val);

	fclose(fp);

	return 0;
}
