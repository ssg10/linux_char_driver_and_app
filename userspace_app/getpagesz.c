#include <unistd.h>
#include <stdio.h>

int main()
{
	int pageSize = getpagesize();
	printf("Page size - %i bytes\n", pageSize);
	return 0;

}
