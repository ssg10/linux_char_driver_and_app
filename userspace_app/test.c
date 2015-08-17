/*
 *
 *  User-space test program for HVDIMM char driver on MMLS
 *
 *  (C) 2015 Netlist, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Author: S.Gosali <sgosali@netlist.com>
 *  August 2015
 *  
 *  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *  NOTE: To build, need to copy hv_cdev_uapi.h into /usr/include/uapi/linux
 *  		It has user-space IOCTL definition.
 *  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *  	
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define _USE_LARGEFILE64
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <uapi/linux/hv_cdev_uapi.h>

#define PATTERN 0x1F
#define BUF_SIZE 1024*1024	/* 1MB */
#define MB_SHIFT 20

#define MEM_TEST 1		/* 1 = enable more mem log and extra mem test 	*/
#define USE_CLFLUSH 1	/* 1 = use cl flush ioctl for mmap test   		*/

char write_buf[BUF_SIZE], read_buf[BUF_SIZE];
int fd;

static void write_file()
{
	int write_len = 0;

	/* Rewind, because file position is maintained in driver */
	lseek(fd, 0, SEEK_SET);

	printf (" Enter the data to be written into device\n");
	scanf (" %[^\n]", write_buf);
	write_len = strlen(write_buf);
	printf("Length of data is %d bytes\n", write_len);
	write(fd, write_buf, sizeof(write_buf));
}

static void read_file()
{
	lseek(fd, 0, SEEK_SET);

    read(fd, read_buf, sizeof(read_buf));
    printf ("The data in the device is %s\n", read_buf);
}

static int dump_mmls_memory( void )
{
	struct hv_mmls_range range;
	int res;
	
	/* Arbitrarily dump 16 bytes for sanity check only */
	range.offset = 0;
	range.size = 16;

	printf("Issue ioctl to dump a few bytes of mmls mem...\n");
	printf("Offset: %#llx  size: %llu\n", range.offset, range.size);
	
	res = ioctl(fd, HV_MMLS_DUMP_MEM, &range);
	
	if (res < 0)
		perror("\nioctl HV_MMLS_FLUSH_RANGE failed !!\n");	
}

static int get_dev_size(uint64_t* size)
{
	int res;
	
	uint64_t dev_size;
	
	printf("Issue ioctl HV_MMLS_SIZE\n");
	res = ioctl(fd, HV_MMLS_SIZE, &dev_size);
	if (res < 0) {
		perror("ioctl HV_MMLS_SIZE failed");
		return -1;
	}
	
	*size = dev_size;
	
	return 0;
}

static void print_menu()
{
	fflush(stdin);
	printf("\n\n");
	printf("----------------------------------\n");
	printf("HVDIMM MMLS char device test app  \n");
	printf("----------------------------------\n\n");
	printf("Press: \n\n");
	printf("1. To read back string data from device \n");
	printf("2. To write string data to  device\n");
	printf("3. <TBD> To read n-bytes from device\n");
	printf("4. <TBD> To write n-bytes to device\n");
	printf("5. <TBD> To run read / write/ compare test\n");
	printf("6. To do mmap, write pattern to entire mmls space, and verify written data\n");
	printf("7. To get mmls disk size via ioctl\n\n");
	printf("q to quit\n ");
}

/*
 * mmap_file()
 * 
 * Call mmap() syscall to enable user to access physical mem 
 * 	from user space DIRECTLY by mapping the mem from kernel space
 * 	to user space.  
 * 	The purpose is to speed up data handling by bypassing
 * 	read() write() ops overhead
 */
static void mmap_file()
{
    void *addr;
    int i, j, res;
    char *rbuf, *wbuf;
	uint64_t mb_size;	/* # of MB segments */
    size_t dev_size; 
    int n;
    
    /* Find out mmls disk size first via ioctl */
    get_dev_size(&dev_size);
    printf("mmls size: %llu\n", dev_size);
    
    fflush(NULL);
    
    wbuf = malloc(BUF_SIZE);
	if (!wbuf) {
		perror("can't allocate write buffer\n");
		exit(EXIT_FAILURE);
	}
	
	printf("wbuf = 0x%p\n", wbuf);
	printf("Setting up buffers with patterns...\n");
	memset(wbuf, PATTERN, BUF_SIZE);

    /* map kernel VA region into user-space VA */
	printf("Calling mmap system call to map kernel VA to user VA...\n");
	
	/* Note: - mmap(,,,,off) -- off must be page aligned */
	/*  	 - MAP_SHARED is used to make data change reflected directly 	*/
	/*			into mmls physical memory. Originally, used MAP_PRIVATE  	*/
	/*          and unknown why ...											*/
	addr = mmap(NULL, dev_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		perror("map failed\n");
		printf("errno = %d\n", errno);
		goto error;
	}

	printf("Mapped to user VA = %p\n", addr);
	printf("Copying write buffer into mmap region...\n");

	unsigned char *p = addr;
	
#if MEM_TEST
	/* Dump a few bytes of what will be written (wbuf) */
	printf("Dump a few bytes of write buf for sanity check:\n");
	for(i = 0; i < 12; i++)
		printf("wbuf[%d]=0x%02X\n",i, wbuf[i]);
	
	/* Test: call write() to check -EFAULT for invalid virt mem addr */
	/* w/o the risk of crashing the system */
	size_t size;
	size_t test_size = 32; 	/* assuming dev_size should be way larger */
							/* test size does not matter, just need find err */
	size = write(fd, p, test_size);
	if (size < test_size) {
		printf("mem error: size = %zd, errno=%d", size, errno);
		printf("mmap-ed address might be invalid! Quitting/n");
		goto error;
	} 
	else
		printf("write() test passed. mem addr valid.\n");
#endif
	
	fflush(NULL);
	
	/* Do copying from wbuf into addr per BUF_SIZE.... */
	printf("p = %p\n", p);
	
	/* figure out how many segments of MB being copied */
	mb_size = dev_size >> MB_SHIFT;
	printf("# MB segments: mb_size = %llu\n", mb_size);
	printf("Copying in progress - every dot is 256MB:\n");
	for (i = 0, j = 0; i < mb_size; i++, j++) {
		memcpy(p, wbuf, BUF_SIZE);
		p += BUF_SIZE;

		if (j == 256) {
			printf(".");
			fflush(NULL);
			j = 0;
		}
	}
	    
	printf("\nCopying done...\n");
	fflush(NULL);

	/* Dump memory in kernel corresponding to mmls to verify */
	dump_mmls_memory();
	
	/* CLflush */
#if USE_CLFLUSH
	struct hv_mmls_range range;

	range.offset = 0;
	range.size = dev_size;

	printf("Issue ioctl to flush cache range\n");
	printf("Offset: %#llx  size: %llu\n", range.offset, range.size);
	
	res = ioctl(fd, HV_MMLS_FLUSH_RANGE, &range);
	if (res < 0)
		perror("\nioctl HV_MMLS_FLUSH_RANGE failed !!\n");
#endif
	
	fflush(NULL);
	
	p = addr;
	printf("p = %p\n", p);
	
#if MEM_TEST	
	/* Dump a few bytes of what has been written to check */
	printf("Dump a few bytes of written data for sanity check:\n");
	for(i = 0; i < 12; i++)
		printf("p[%d]=0x%02X\n",i, p[i]);
#endif

	/* Memory test to check if whatever written was good */
	printf("Memory check: Comparing write buffer with mmap data...\n");
	printf("Testing each byte of previously written data: \n\n");
	
	for (i = 0, j = 0; i < mb_size; i++, j++) {
		res = memcmp(p, wbuf, BUF_SIZE);
		if (res != 0) {
			fprintf(stderr, "compare failed at %d MB !\n", i);
			printf("error when comparing ! quit\n");
			goto error;
		}
		p += BUF_SIZE;
		
		if (j == 256) {
			printf(".");
			fflush(NULL);
			j = 0;
		}
	}

	printf("\nTest PASSED...\n");
	fflush(NULL);
	
error:
	printf("Unmapped and back to menu... \n");
	munmap(addr, dev_size);
	free(wbuf);
	return;
}

main ( )
{
        int i, res;
        char ch;
        char input[30];
        int nbytes;
        uint64_t dev_size;

        printf("Opening /dev/hv_cdev0 file\n");
        fd = open64("/dev/hv_cdev0", O_RDWR);
        if (fd == -1)
        {
                printf("Error in opening file \n");
                return(-1);
        }

        while(1) {
        	print_menu();

        	/* read user input and get rid of CR */
        	scanf ("%s%*c", &ch);
        	printf("Your input = %c\n\n", ch);

        	switch (ch) {
        		case '1':
            		read_file();
                    break;

                case '2':
                	write_file();
                    break;

                case '3':
                	printf("How many bytes to read? ");
                	fgets(input, sizeof(input), stdin);
                	printf("Your input = %d\n", atoi(input));
                	break;

                case '4':
                	printf("How many bytes to write? ");
                	fgets(input, sizeof(input), stdin);
                	printf("Your input = %d\n", atoi(input));
                	break;
                	
                case '6':
                	fflush(NULL); 	
                	mmap_file();
                	break;
                	
                case '7':
                	get_dev_size(&dev_size);
                	printf("mmls size: %llu\n", dev_size);
                	break;
                	
                case 'q':
                	goto exit;

                default:
                    printf("\nWrong choice \n");
                    break;
        	}
        }

 exit:
        close(fd);
        return 0;
}