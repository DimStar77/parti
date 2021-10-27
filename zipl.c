#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <iconv.h>
#include <getopt.h>
#include <inttypes.h>
#include <uuid/uuid.h>
#include <blkid/blkid.h>
#include <json-c/json.h>

#include "disk.h"
#include "filesystem.h"
#include "util.h"
#include "zipl.h"

#define ZIPL_MAGIC      "zIPL"
#define ZIPL_PSW_MASK   0x000000007fffffffll
#define ZIPL_PSW_LOAD   0x0008000080000000ll

// FIXME
struct { struct { unsigned raw:1; } show; } opt;

void dump_zipl_components(disk_t *disk, uint64_t sec)
{
  unsigned char buf[disk->block_size];
  unsigned char buf2[disk->block_size];
  unsigned char buf3[disk->block_size];
  int i, k;
  uint64_t start, load, start2;
  unsigned size, type, size2, len2;
  char *s;
  zipl_stage3_head_t zh = {};

  i = disk_read(disk, buf, sec, 1);

  // compare including terminating 0 (header type 0 = ZIPL_COMP_HEADER_IPL)
  if(i || memcmp(buf, ZIPL_MAGIC, sizeof ZIPL_MAGIC)) {
    printf("       no components\n");
    return;
  }

  for(i = 1; i < disk->block_size/32; i++) {
    start = read_qword_be(buf + i * 0x20);
    size = read_word_be(buf + i * 0x20 + 8);
    type = read_byte(buf + i * 0x20 + 0x17);
    load = read_qword_be(buf + i * 0x20 + 0x18);
    if(!load) break;
    printf("       %u start %llu", i - 1, (unsigned long long) start);
    if((size != disk->block_size && type == 2) || opt.show.raw) printf(", blksize %d", size);
    printf(
      ", addr 0x%016llx, type %d%s\n",
      (unsigned long long) load,
      type,
      type == 1 ? " (exec)" : type == 2 ? " (load)" : ""
    );
    if(type == 2) {
      k = disk_read(disk, buf2, start, 1);
      if(!k) {
        for(k = 0; k < disk->block_size/32; k++) {
          start2 = read_qword_be(buf2 + k * 0x10);
          size2 = read_word_be(buf2 + k * 0x10 + 8);
          len2 = read_word_be(buf2 + k * 0x10 + 10) + 1;
          if(!start2) break;
          printf(
            "         => start %llu, size %u",
            (unsigned long long) start2,
            len2
          );
          if(size2 != disk->block_size || opt.show.raw) printf(", blksize %d", size2);
          if((s = iso_block_to_name(disk, start2))) {
            printf(", \"%s\"", s);
          }
          printf("\n");
        }

        // read it again
        start2 = read_qword_be(buf2);

        if(start2) {
          if(load == 0xa000 && !disk_read(disk, buf3, start2, 1)) {
            zh.parm_addr = read_qword_be(buf3);
            zh.initrd_addr = read_qword_be(buf3 + 8);
            zh.initrd_len = read_qword_be(buf3 + 0x10);
            zh.psw = read_qword_be(buf3 + 0x18);
            zh.extra = read_qword_be(buf3 + 0x20);
            zh.flags = read_word_be(buf3 + 0x28);
            zh.ok = 1;
            printf(
              "         <zIPL stage3>\n"
              "            parm 0x%016llx, initrd 0x%016llx (len %llu)\n"
              "            psw 0x%016llx, extra %llu, flags 0x%x\n",
              (unsigned long long) zh.parm_addr,
              (unsigned long long) zh.initrd_addr,
              (unsigned long long) zh.initrd_len,
              (unsigned long long) zh.psw,
              (unsigned long long) zh.extra,
              zh.flags
            );
          }

          if((load | ZIPL_PSW_LOAD) == zh.psw ) {
            printf("         <kernel>\n");
          }

          if(load == zh.initrd_addr ) {
            printf("         <initrd>\n");
          }

          if(load == zh.parm_addr ) {
            printf("         <parm>\n");
            if(!disk_read(disk, buf3, start2, 1)) {
              unsigned char *s = buf3;
              buf3[sizeof buf3 - 1] = 0;
              printf("            \"");
              while(*s) {
                if(*s == '\n') {
                  printf("\\n");
                }
                else if(*s == '\\' || *s == '"'/*"*/) {
                  printf("\\%c", *s);
                }
                else if(*s >= 0x20 && *s < 0x7f) {
                  printf("%c", *s);
                }
                else {
                  printf("\\x%02x", *s);
                }
                s++;
              }
              printf("\"\n");
            }
          }
        }
      }
    }

    if(type == 1) {
      if(load == (ZIPL_PSW_LOAD | 0xa050)) {
        printf("         <zipl stage3 entry>\n");
      }
    }
  }
}


void dump_zipl(disk_t *disk)
{
  int i;
  unsigned char buf[disk->block_size = 0x200];
  uint64_t pt_sec, sec;
  unsigned size;
  char *s;

  i = disk_read(disk, buf, 0, 1);

  if(i || memcmp(buf, ZIPL_MAGIC, sizeof ZIPL_MAGIC - 1)) return;

  printf(SEP "\nzIPL (SCSI scheme):\n");

  printf(
    "  sector size: %d\n  version: %u\n",
    disk->block_size,
    read_dword_be(buf + 4)
  );

  pt_sec = read_qword_be(buf + 0x10);
  size = read_word_be(buf + 0x18);

  printf("  program table: %llu", (unsigned long long) pt_sec);
  if(size != disk->block_size || opt.show.raw) printf(", blksize %u", size);
  if((s = iso_block_to_name(disk, pt_sec))) {
    printf(", \"%s\"", s);
  }
  printf("\n");

  i = disk_read(disk, buf, pt_sec, 1);

  if(i || memcmp(buf, ZIPL_MAGIC, sizeof ZIPL_MAGIC - 1)) {
    printf("  invalid program table\n");
    return;
  }

  for(i = 1; i < disk->block_size/16; i++) {
    sec = read_qword_be(buf + i * 0x10);
    size = read_word_be(buf + i * 0x10 + 8);
    if(!sec) break;
    printf("  %-3d  start %llu", i - 1, (unsigned long long) sec);
    if(size != disk->block_size || opt.show.raw) printf(", blksize %u", size);
    printf(", components:\n");
    dump_zipl_components(disk, sec);
  }
}