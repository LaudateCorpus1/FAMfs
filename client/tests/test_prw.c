/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 * Written by
 * 	Teng Wang tw15g@my.fsu.edu
 * 	Adam Moody moody20@llnl.gov
 * 	Weikuan Yu wyu3@fsu.edu
 * 	Kento Sato kento@llnl.gov
 * 	Kathryn Mohror. kathryn@llnl.gov
 * 	LLNL-CODE-728877.
 * All rights reserved.
 *
 * This file is part of BurstFS For details, see https://github.com/llnl/burstfs.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *
 * Copyright (c) 2013, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * code Written by
 *   Raghunath Rajachandrasekar <rajachan@cse.ohio-state.edu>
 *   Kathryn Mohror <kathryn@llnl.gov>
 *   Adam Moody <moody20@llnl.gov>
 * All rights reserved.
 * This file is part of CRUISE.
 * For details, see https://github.com/hpc/cruise
 * Please also read this file COPYRIGHT
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <mpi.h>
#include <sys/time.h>
#include <aio.h>
#include <sys/mman.h>

#include <unifycr.h>

#define GEN_STR_LEN 1024

#define print0(...) if (rank == 0 && vmax == 0) {printf(__VA_ARGS__);}
#define printv(...) if (vmax > 0) {printf(__VA_ARGS__);}

struct timeval read_start, read_end;
double read_time = 0;

struct timeval write_start, write_end;
double write_time = 0;

struct timeval meta_start, meta_end;
double meta_time = 0;

typedef struct {
  int fid;
  long offset;
  long length;
  char *buf;

}read_req_t;

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

long getval(char *str) {
    char *sfx;
    size_t rv = strtoul(str, &sfx, 10);

    switch (*sfx) {
    case 'G':
    case 'g':
        rv *= 1024*1024*1024L;
        break;
    case 'M':
    case 'm':
        rv *= 1024*1024L;
        break;
    case 'K':
    case 'k':
        rv *= 1024L;
        break;
    }

    return rv;
}

int main(int argc, char *argv[]) {

    static const char * opts = "b:s:t:f:p:u:M:D:S:w:r:i:v:W:GR";
    char tmpfname[GEN_STR_LEN+11], fname[GEN_STR_LEN];
    long blk_sz, seg_num, tran_sz = 1024*1024, read_sz = tran_sz;
    //long num_reqs;
    int pat, c, rank_num, rank, fd, \
            to_unmount = 0;
    int mount_burstfs = 1, direct_io = 0, sequential_io = 0, write_only = 0;
    int initialized, provided, rrc = MPI_SUCCESS;
    int gbuf = 0, mreg = 0;
    off_t ini_off;
    void *rid;
    int vmax = 0;
    size_t warmup = 0;

    //MPI_Init(&argc, &argv);
    MPI_Initialized(&initialized);
    if (!initialized) {
        rrc = MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    }

    if (rrc != MPI_SUCCESS) {
        printf("MPI_Init_thread failed\n");
        exit(1);
    }

    rrc = MPI_Comm_size(MPI_COMM_WORLD, &rank_num);
    if (rrc != MPI_SUCCESS) {
        printf("MPI_Comm_size failed\n");
        exit(1);
    }

    rrc = MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rrc != MPI_SUCCESS) {
        printf("MPI_Comm_rank failed\n");
        exit(1);
    }
    print0("startng test_pwr\n");

#define ULFS_MAX_FILENAME 128
    char hostname[ULFS_MAX_FILENAME] = {0};
    gethostname(hostname, ULFS_MAX_FILENAME);
    printv("%s: Rank=%u, Rank num=%u\n", hostname, rank, rank_num);

    while((c = getopt(argc, argv, opts)) != -1) {
        switch (c)  {
            case 'b': /*size of block*/
               blk_sz = getval(optarg); break;
            case 's': /*number of blocks each process writes*/
               seg_num = getval(optarg); break;
            case 't': /*size of each write*/
               tran_sz = getval(optarg); break;
            case 'r': /*size of each read*/
               read_sz= getval(optarg); break;
            case 'f':
               strcpy(fname, optarg); break;
            case 'p':
               pat = atoi(optarg); break; /* 0: N-1 segment/strided, 1: N-N*/
            case 'u':
               to_unmount = atoi(optarg); break; /*0: not unmount after finish 1: unmount*/
            case 'M':
               mount_burstfs = atoi(optarg); break; /* 0: Don't mount burstfs */
            case 'D':
               direct_io = atoi(optarg); break; /* 1: Open with O_DIRECT */
            case 'S':
               sequential_io = atoi(optarg); break; /* 1: Write/read blocks sequentially */
            case 'w':
               write_only = atoi(optarg); break;
            case 'v':
               vmax = atoi(optarg); break;
            case 'G':
               gbuf++; break;
            case 'R':
               mreg++; break;
            case 'i':
               ini_off = getval(optarg); break;   /* 1st write initial offset: simulate unaligned writes */
            case 'W':
               warmup = getval(optarg); break;
        }
    }

    if (rank == 0) printf(" %s, %s, %s I/O, %s block size:%ldW/%ldR segment:%ld hdr off=%lu\n",
        (pat)? "N-N" : ((seg_num > 1)? "strided":"segmented"),
        (direct_io)? "direct":"buffered",
        (sequential_io)? "sequential":"randomized",
        (write_only)? "W":"W/R",
        tran_sz, read_sz, blk_sz, ini_off);

    if (mount_burstfs) {
        print0("mount unifycr\n");
        unifycr_mount("/tmp/mnt", rank, rank_num, 0, 3);
    } else
        to_unmount = 0;

    char *buf;
    size_t len;
    if (gbuf) {
        len = blk_sz*seg_num;
        buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    } else {
        len = max(tran_sz, read_sz);
        posix_memalign((void**)&buf, getpagesize(), len);
    }


    if (buf == NULL || buf == MAP_FAILED) {
        printf("[%02d] can't allocate %luMiB of  memory\n", rank, len/1024/1024);
        return -1;
    }
    memset(buf, 0, max(tran_sz, read_sz));

    if (warmup) {
        print0("warming up...\n");
        printv("%02d warming up\n", rank);
        sprintf(tmpfname, "%s-%d.warmup", fname, rank);
        fd = open(tmpfname, O_RDWR | O_CREAT);
        if (fd < 0) {
            printf("%02d warm-up file %s open failure\n", rank, fname);
            exit(1);
        }
        while (warmup) {
            ssize_t l = write(fd, buf, tran_sz);

            if (l < 0) {
                printf("%02d warm-up file %s write error\n", rank, fname);
                exit(1);
            }
            warmup -= l;
        }
        close(fd);
    }

    if (mreg) {
       int rc = famfs_buf_reg(buf, len, &rid);
       if (rc) {
           printf("%02d buf register error %d\n", rank, rc);
           return -1;
       }
    }

    MPI_Barrier(MPI_COMM_WORLD);

    if (pat == 1) {
        sprintf(tmpfname, "%s%d", fname, rank);
    } else {
        sprintf(tmpfname, "%s", fname);
    }

    print0("opening files\n");

    int flags = O_RDWR | O_CREAT | O_TRUNC;
    if (direct_io)
        flags |= O_DIRECT;
    fd = open(tmpfname, flags, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        printf("%02d open file %s failure\n", rank, tmpfname);
        fflush(stdout);
        return -1;
    }
    if (direct_io)
        fsync(fd);
   
    long i, j; 
    unsigned long offset, rc, lw_off = 0, *p;
    char *bufp = buf;
    offset = 0;

    print0("writing...\n");
    printv("%02d writing\n", rank);
    gettimeofday(&write_start, NULL);

    for (i = 0; i < seg_num; i++) {
        long jj;
        for (jj = 0; jj < blk_sz/tran_sz; jj++) {
            if (sequential_io) {
                j = jj;
            } else {
                j = (blk_sz/tran_sz - 1) - 2*jj; /* reverse */
                if (j < 0)
                    j += (blk_sz/tran_sz - 1);
            }
            if (pat == 0)
                offset = i*rank_num*blk_sz + rank*blk_sz + j*tran_sz;
            else if (pat == 1)
                offset = i*blk_sz + j*tran_sz;

            int k;
            lw_off = 0;

            if (gbuf) 
                bufp = buf + i*blk_sz + j*tran_sz;

            for (k = 0; k < tran_sz/sizeof(unsigned long); k++) {
                if (gbuf) 
                    p = &(((unsigned long *)bufp)[k]);
                else
                    p = &(((unsigned long*)buf)[k]);

                //*p = offset + k;
                *p = offset + lw_off*sizeof(long);
                lw_off++;
            }

            rc = pwrite(fd, bufp, tran_sz, offset);
            if (rc < 0) {
                printf("%02d write failure\n", rank);
                fflush(stdout);
                return -1;
            }
        }
    }

    printv("%02d syncing\n", rank);

    gettimeofday(&meta_start, NULL);
    fsync(fd);
    gettimeofday(&meta_end, NULL);
    meta_time += 1000000*(meta_end.tv_sec - meta_start.tv_sec) + 
        meta_end.tv_usec - meta_start.tv_usec;
    meta_time /= 1000000;
    gettimeofday(&write_end, NULL);
    write_time += 1000000*(write_end.tv_sec - write_start.tv_sec) + 
        write_end.tv_usec - write_start.tv_usec;
    write_time = write_time/1000000;


    close(fd);
    if (direct_io) {
        MPI_Barrier(MPI_COMM_WORLD);
        printf("%s: drop_caches\n", hostname);
        system("echo 1 > /proc/sys/vm/drop_caches");
    }
    MPI_Barrier(MPI_COMM_WORLD);
    print0("closed files\n");


    double write_bw = (double)blk_sz*seg_num/1048576/write_time;
    double agg_write_bw;
    MPI_Reduce(&write_bw, &agg_write_bw, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    double max_write_time;
    MPI_Reduce(&write_time, &max_write_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    double min_write_bw;
    min_write_bw=(double)blk_sz*seg_num*rank_num/1048576/ max_write_time;

    double agg_meta_time;
    MPI_Reduce(&meta_time, &agg_meta_time,  1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    double max_meta_time;
    MPI_Reduce(&meta_time, &max_meta_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        printf("Aggregate Write BW is %lfMB/s, Min Write BW is %lfMB/s\n",
                agg_write_bw, min_write_bw);
        printf("Per-process sync time %lf sec, Max %lf sec\n",
                agg_meta_time / rank_num, max_meta_time);
        fflush(stdout);
    }
    //free(buf);

    //MPI_Finalize();
    MPI_Barrier(MPI_COMM_WORLD);
    if (write_only) {
        MPI_Finalize();
        exit(rc);
    }

    //num_reqs = blk_sz*seg_num/tran_sz;
    //char *read_buf = malloc(blk_sz * seg_num); /*read buffer*/
    //char *read_buf; /*read buffer*/
    /*
    if (direct_io)
            posix_memalign((void**)&read_buf, getpagesize(), blk_sz);
    else
            read_buf = malloc(blk_sz);

    if (to_unmount) {
            unifycr_mount("/tmp/mnt", rank, rank_num,\
                    0, 3);
    }
    */

    if (pat == 1) {
        sprintf(tmpfname, "%s%d", fname, rank);
    }	else {
        sprintf(tmpfname, "%s", fname);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (direct_io)
        flags = O_RDWR | O_DIRECT;
    else
        flags = O_RDONLY;

    print0("open for read\n");

    fd = open(tmpfname, flags, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        printf("%02d open file failure\n", rank);
        fflush(stdout);
        return -1;
    }
    if (direct_io)
        fsync(fd);

    gettimeofday(&read_start, NULL);

    print0("reading...\n");
    printv("%02d reading\n", rank);

    long vcnt, e = 0;
    //long cursor;
    offset = 0;
    for (i = 0; i < seg_num; i++) {
        //cursor = 0;
        long jj;
        for (jj = 0; jj < blk_sz/read_sz; jj++) {
            if (sequential_io) {
                j = jj;
            } else {
                j = (blk_sz/read_sz - 1) - 2*jj; /* reverse */
                if (j < 0)
                    j += (blk_sz/read_sz - 1);
            }
            if (pat == 0)
                offset = i*rank_num*blk_sz + rank*blk_sz + j*read_sz;
            else if (pat == 1)
                offset = i*blk_sz + j*read_sz;

            //cursor = j * read_sz;
            //rc = pread(fd, read_buf + cursor, read_sz, offset);

            int k;
            lw_off = 0;

            if (gbuf) 
                bufp = buf + i*blk_sz + j*read_sz;

            rc = pread(fd, bufp, read_sz, offset);
            if (rc < 0) {
                printf("%02d read failure\n", rank);
                fflush(stdout);
                return -1;
            }

            vcnt = 0;
            for (k = 0; k < read_sz/sizeof(unsigned long); k++) {
                //unsigned long *p = &(((unsigned long*)(read_buf + cursor))[k]);
               
                if (gbuf) 
                    p = &(((unsigned long *)bufp)[k]);
                else
                    p = &(((unsigned long*)buf)[k]);

                if (*p != offset + (lw_off*sizeof(long))) {
                    e++;
                    if (vcnt < vmax) {
                        printf("DATA MISMATCH @%lu, expected %lu, got %lu [%u]\n", offset, offset + (lw_off*sizeof(long)), *p, k*8);
                        vcnt++;
                    }
#if 0
                    int ii, jj;
                    for (ii = 0; ii < 16; ii++) {
                        printf("### %02d: ", ii);
                        for (jj = 0; jj < 8; jj++) {
                            //printf("[%02x] ", (unsigned char)*(read_buf + cursor + ii*8 + jj));
                            printf("[%02x] ", (unsigned char)*(buf + ii*8 + jj));
                        }
                        printf("\n");
                    }
                    break;
#endif
                }
                lw_off++;
            }
        }
    }

    if (e)
        printf("%02d: %ld data verification errors\n", rank, e);
    else 
        printv("%02d success\n", rank);

    gettimeofday(&read_end, NULL);
    read_time = (read_end.tv_sec - read_start.tv_sec)*1000000 + read_end.tv_usec - read_start.tv_usec;
    read_time = read_time/1000000;

    close(fd);
    MPI_Barrier(MPI_COMM_WORLD);

    if (mreg)
        famfs_buf_unreg(rid);

    //free(read_buf);
    if (gbuf)
        munmap(buf, len);
    else
        free(buf);

    double read_bw = (double)blk_sz*seg_num/1048576/read_time;
    double agg_read_bw;

    double max_read_time, min_read_bw;
    MPI_Reduce(&read_bw, &agg_read_bw, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&read_time, &max_read_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);


    min_read_bw=(double)blk_sz*seg_num*rank_num/1048576/max_read_time;
    if (rank == 0) {
        printf("Aggregate Read BW is %lfMB/s, Min Read BW is %lf\n", agg_read_bw,  min_read_bw);
        fflush(stdout);
    }

    // *** this will only free libfabric context and WILL NOT shut down servers
    if (to_unmount)
        ;
    unifycr_unmount(); 

    MPI_Finalize();
    exit(rc);
}
