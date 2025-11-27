#include <stdio.h>
#include <stdint.h>
#include <unistd.h>


int main(int argc, const char *argv[]){
	uint32_t sample = 0;
	for(;;){
		write(STDOUT_FILENO, &sample, sizeof(sample));
		sample++;
	}
	return(0);
}

