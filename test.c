#include <stdio.h>
#include <stdlib.h>

int main() {
	FILE *fp;
	fp = fopen("/dev/PiPlates", "r");
	if(fp == NULL){
		printf("error!");
		exit(1);
	}

	fclose(fp);

	return 0;
}
