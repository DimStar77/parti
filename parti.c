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

#define EFI_MAGIC	0x5452415020494645ll
#define APPLE_MAGIC	0x504d
#define ISO_MAGIC	"\001CD001\001"
#define ZIPL_MAGIC	"zIPL"
#define ELTORITO_MAGIC	"\000CD001\001EL TORITO SPECIFICATION"
#define ZIPL_PSW_MASK	0x000000007fffffffll
#define ZIPL_PSW_LOAD	0x0008000080000000ll


#ifndef VERSION
#define VERSION "0.0"
#endif

typedef struct {
  unsigned type;
  unsigned boot:1;
  unsigned valid:1;
  unsigned empty:1;
  struct {
    unsigned c, h, s, lin;
  } start;
  struct {
    unsigned c, h, s, lin;
  } end;
  unsigned real_base;
  unsigned base;
  unsigned idx;
} ptable_t;

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc;
  uint32_t reserved;
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_lba;
  uint64_t last_lba;
  uuid_t disk_guid;
  uint64_t partition_lba;
  uint32_t partition_entries;
  uint32_t partition_entry_size;
  uint32_t partition_crc;
} gpt_header_t;

typedef struct {
  uint16_t signature;
  uint16_t reserved1;
  uint32_t partitions;
  uint32_t start;
  uint32_t size;
  char name[32];
  char type[32];
  uint32_t data_start;
  uint32_t data_size;
  uint32_t status;
} apple_entry_t;

typedef struct {
  uuid_t type_guid;
  uuid_t partition_guid;
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t attributes;
  uint16_t name[36];
} gpt_entry_t;

typedef union {
  struct {
    uint8_t header_id;
    uint8_t reserved[31];
  } any;

  struct {
    uint8_t header_id;
    uint8_t platform_id;
    uint16_t reserved1;
    char name[24];
    uint16_t crc;
    uint16_t magic;
  } validation;

  struct {
    uint8_t header_id;
    uint8_t info;
    uint8_t reserved[30];
  } extension;

  struct {
    uint8_t header_id;
    uint8_t media;
    uint16_t load_segment;
    uint8_t system;
    uint8_t reserved;
    uint16_t size;
    uint32_t start;
    uint8_t criteria;
    char name[19];
  } entry;

  struct {
    uint8_t header_id;
    uint8_t platform_id;
    uint16_t entries;
    char name[28];
  } section;
} eltorito_t;


// note: is not supposed to not match binary layout!
typedef struct {
  uint64_t parm_addr;
  uint64_t initrd_addr;
  uint64_t initrd_len;
  uint64_t psw;
  uint64_t extra;	// 1 = scsi, 0 = !scsi
  unsigned flags;	// bit 0: scsi, bit 1: kdump
  unsigned ok:1;	// 1 = zipl stage 3 header read
} zipl_stage3_head_t;

json_object *json_root;

void help(void);
uint32_t chksum_crc32(void *buf, unsigned len);
unsigned read_byte(void *buf);
unsigned read_word_le(void *buf);
unsigned read_word_be(void *buf);
unsigned read_dword_le(void *buf);
unsigned read_dword_be(void *buf);
uint64_t read_qword_le(void *buf);
uint64_t read_qword_be(void *buf);
unsigned cs2s(unsigned cs);
unsigned cs2c(unsigned cs);
char *efi_partition_type(char *guid);
char *mbr_partition_type(unsigned id);
char *efi_guid_decode(uuid_t guid);
char *utf32_to_utf8(uint32_t u8);
void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries);
int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h);
void print_ptable_entry(disk_t *disk, int nr, ptable_t *ptable);
int is_ext_ptable(ptable_t *ptable);
ptable_t *find_ext_ptable(ptable_t *ptable, int entries);
void dump_mbr_ptable(disk_t *disk);
uint64_t dump_gpt_ptable(disk_t *disk, uint64_t addr);
void dump_gpt_ptables(disk_t *disk);
void dump_apple_ptables(disk_t *disk);
int dump_apple_ptable(disk_t *disk);
void dump_eltorito(disk_t *disk);
void dump_zipl_components(disk_t *disk, uint64_t sec);
void dump_zipl(disk_t *disk);


struct option options[] = {
  { "help",        0, NULL, 'h'  },
  { "verbose",     0, NULL, 'v'  },
  { "raw",         0, NULL, 1001 },
  { "version",     0, NULL, 1002 },
  { "disk-export", 1, NULL, 1003 },
  { "disk-import", 1, NULL, 1004 },
  { "json",        1, NULL, 1005 },
  { }
};

struct {
  unsigned verbose;
  struct {
    unsigned raw:1;
  } show;
  char *export_file;
  char *json_file;
} opt;


int main(int argc, char **argv)
{
  int i;
  extern int optind;
  extern int opterr;

  opterr = 0;

  while((i = getopt_long(argc, argv, "hv", options, NULL)) != -1) {
    switch(i) {
      case 'v':
        opt.verbose++;
        break;

      case 1001:
        opt.show.raw = 1;
        break;

      case 1002:
        printf(VERSION "\n");
        return 0;
        break;

      case 1003:
        opt.export_file = optarg;
        break;

      case 1004:
        disk_import(optarg);
        break;

      case 1005:
        opt.json_file = optarg;
        break;

      default:
        help();
        return i == 'h' ? 0 : 1;
    }

  }

  argc -= optind;
  argv += optind;

  while(*argv) disk_init(*argv++);

  if(!disk_list_size) {
    help();
    exit(1);
  }

  json_root = json_object_new_object();

  for(unsigned u = 0; u < disk_list_size; u++) {
    fs_detail(disk_list + u, 0, 0);
    dump_mbr_ptable(disk_list + u);
    dump_gpt_ptables(disk_list + u);
    dump_apple_ptables(disk_list + u);
    dump_eltorito(disk_list + u);
    dump_zipl(disk_list + u);
  }

  if(opt.export_file) {
    unlink(opt.export_file);
    for(unsigned u = 0; u < disk_list_size; u++) {
      disk_export(disk_list + u, opt.export_file);
    }
  }

  if(opt.json_file) {
    if(strcmp(opt.json_file, "-")) {
      json_object_to_file_ext(opt.json_file, json_root, JSON_C_TO_STRING_PRETTY);
    }
    else {
      json_object_to_fd(STDOUT_FILENO, json_root, JSON_C_TO_STRING_PRETTY);
      printf("\n");
    }
  }

  json_object_put(json_root);

  return 0;
}


void help()
{
  fprintf(stderr,
    "Partition Info " VERSION "\nUsage: parti [OPTIONS] DISKIMAGE\n"
    "\n"
    "Options:\n"
    "  --verbose     show more data\n"
    "  --version     show version\n"
    "  --help        show this text\n"
  );
}


uint32_t chksum_crc32(void *buf, unsigned len)
{
  static uint32_t crc_tab[256] = {
    0,          0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
  };

  uint32_t crc;
  unsigned u;
  unsigned char *p = buf;

#if 0
  for(u = 0, crc = 0xffffffff; u < len; u++) {
    crc = crc_tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
#else
  for(u = 0, crc = 0xffffffff; u < len; u++) {
    crc = crc_tab[(uint8_t) crc ^ *p++] ^ (crc >> 8);
  }
#endif

  return crc ^ 0xffffffff;
}


unsigned cs2s(unsigned cs)
{
  return cs & 0x3f;
}


unsigned cs2c(unsigned cs)
{
  return ((cs >> 8) & 0xff) + ((cs & 0xc0) << 2);
}


char *efi_guid_decode(uuid_t guid)
{
  uuid_t uuid;
  static char buf[37];
  static unsigned char idx[sizeof (uuid_t)] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
  int i;
  unsigned char *s, *d;

  s = (unsigned char *) guid;
  d = (unsigned char *) uuid;

  for(i = 0; i < sizeof uuid; i++) {
    d[i] = s[idx[i]];
  }

  uuid_unparse_lower(uuid, buf);

  return buf;
}


char *utf32_to_utf8(uint32_t u8)
{
  static char buf[16];
  static iconv_t ic = (iconv_t) -1;
  char *ibuf, *obuf;
  size_t obuf_left, ibuf_left;
  int i;

  *buf = 0;

  if(ic == (iconv_t) -1) {
    ic = iconv_open("utf8", "utf32le");
    if(ic == (iconv_t) -1) {
      fprintf(stderr, "oops: can't convert utf8 data\n");
      return buf;
    }
  }

  ibuf = (char *) &u8;
  obuf = buf;
  ibuf_left = 4;
  obuf_left = sizeof buf - 1;

  i = iconv(ic, &ibuf, &ibuf_left, &obuf, &obuf_left);

  if(i >= 0) {
    i = sizeof buf - 1 - obuf_left;
    buf[i] = 0;
  }
  else {
    buf[0] = '.';
    buf[1] = 0;
  }

  return buf;
}

void parse_ptable(void *buf, unsigned addr, ptable_t *ptable, unsigned base, unsigned ext_base, int entries)
{
  unsigned u;

  memset(ptable, 0, entries * sizeof *ptable);

  for(; entries; entries--, addr += 0x10, ptable++) {
    ptable->idx = 4 - entries + 1;
    if(read_qword_le(buf + addr) == 0 && read_qword_le(buf + addr + 0x8) == 0) {
      ptable->empty = 1;
    }
    u = read_byte(buf + addr);
    if(u & 0x7f) continue;
    ptable->boot = u >> 7;
    ptable->type = read_byte(buf + addr + 4);
    u = read_word_le(buf + addr + 2);
    ptable->start.c = cs2c(u);
    ptable->start.s = cs2s(u);
    ptable->start.h = read_byte(buf + addr + 1);
    ptable->start.lin = read_dword_le(buf + addr + 8);
    u = read_word_le(buf + addr + 6);
    ptable->end.c = cs2c(u);
    ptable->end.s = cs2s(u);
    ptable->end.h = read_byte(buf + addr + 5);
    ptable->end.lin = ptable->start.lin + read_dword_le(buf + addr + 0xc);

    ptable->real_base = base;
    ptable->base = is_ext_ptable(ptable) ? ext_base : base;

    if(ptable->end.lin != ptable->start.lin && ptable->start.s && ptable->end.s) {
      ptable->valid = 1;
      ptable->end.lin--;
    }
  }
}


int guess_geo(ptable_t *ptable, int entries, unsigned *s, unsigned *h)
{
  unsigned sectors, heads, u, c;
  int i, ok, cnt;

  for(sectors = 63; sectors; sectors--) {
    for(heads = 255; heads; heads--) {
      ok = 1;
      for(cnt = i = 0; i < entries; i++) {
        if(!ptable[i].valid) continue;

        if(ptable[i].start.h >= heads) { ok = 0; break; }
        if(ptable[i].start.s > sectors) { ok = 0; break; }
        if(ptable[i].start.c >= 1023) {
          c = ((ptable[i].start.lin + 1 - ptable[i].start.s)/sectors - ptable[i].start.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].start.c;
        }
        u = (c * heads + ptable[i].start.h) * sectors + ptable[i].start.s - 1;
        if(u != ptable[i].start.lin) {
          ok = 0;
          break;
        }
        cnt++;
        if(ptable[i].end.h >= heads) { ok = 0; break; }
        if(ptable[i].end.s > sectors) { ok = 0; break; }
        if(ptable[i].end.c >= 1023) {
          c = ((ptable[i].end.lin + 1 - ptable[i].end.s)/sectors - ptable[i].end.h)/heads;
          if(c < 1023) { ok = 0; break; }
        }
        else {
          c = ptable[i].end.c;
        }
        u = (c * heads + ptable[i].end.h) * sectors + ptable[i].end.s - 1;
        if(u != ptable[i].end.lin) {
          ok = 0;
          break;
        }
        cnt++;
      }
      if(!cnt) ok = 0;
      if(ok) break;
    }
    if(ok) break;
  }

  if(ok) {
    *h = heads;
    *s = sectors;
  }

  return ok;
}


void print_ptable_entry(disk_t *disk, int nr, ptable_t *ptable)
{
  unsigned u;

  if(ptable->valid) {
    if(nr > 4 && is_ext_ptable(ptable)) {
      if(!opt.verbose) return;
      printf("    >");
    }
    else {
      printf("  %-3d", nr);
    }

    u = opt.show.raw ? 0 : ptable->base;

    printf("%c %u - %u (size %u), chs %u/%u/%u - %u/%u/%u",
      ptable->boot ? '*' : ' ',
      ptable->start.lin + u,
      ptable->end.lin + u,
      ptable->end.lin - ptable->start.lin + 1,
      ptable->start.c, ptable->start.h, ptable->start.s,
      ptable->end.c, ptable->end.h, ptable->end.s
    );

    if(opt.verbose) {
      printf(", [ref %d.%d]", ptable->real_base, ptable->idx);
    }

    if(opt.show.raw && ptable->base) printf(", ext base %+d", ptable->base);
    printf("\n");

    printf("       type 0x%02x", ptable->type);
    char *s = mbr_partition_type(ptable->type);
    if(s) printf(" (%s)", s);
    printf("\n");
    fs_detail(disk, 7, (unsigned long long) ptable->start.lin + ptable->base);
  }
  else if(!ptable->empty) {
    printf("  %-3d  invalid data\n", nr);
  }
}


int is_ext_ptable(ptable_t *ptable)
{
  return ptable->type == 5 || ptable->type == 0xf;
}


ptable_t *find_ext_ptable(ptable_t *ptable, int entries)
{
  for(; entries; entries--, ptable++) {
    if(ptable->valid && is_ext_ptable(ptable)) return ptable;
  }
  return NULL;
}


void dump_mbr_ptable(disk_t *disk)
{
  int i, j, pcnt, link_count;
  ptable_t ptable[4], *ptable_ext;
  unsigned s, h, ext_base, id;
  unsigned char buf[disk->block_size];

  i = disk_read(disk, buf, 0, 1);

  if(i || read_word_le(buf + 0x1fe) != 0xaa55) {
    // printf("no mbr partition table\n");
    return;
  }

  for(i = j = 0; i < 4 * 16; i++) {
    j |= read_byte(buf + 0x1be + i);
  }

  // empty partition table
  if(!j) return;

  parse_ptable(buf, 0x1be, ptable, 0, 0, 4);
  i = guess_geo(ptable, 4, &s, &h);
  if(!i) {
    s = 63;
    h = 255;
  }
  disk->sectors = s;
  disk->heads = h;
  disk->cylinders = disk->size / (disk->sectors * disk->heads);

  id = read_dword_le(buf + 0x1b8);
  printf(SEP "\nmbr id: 0x%08x\n", id);

  printf("  sector size: %u\n", disk->block_size);

  if(memmem(buf, disk->block_size, "isolinux.bin", sizeof "isolinux.bin" - 1)) {
    char *s;
    unsigned start = le32toh(*(uint32_t *) (buf + 0x1b0));
    printf("  isolinux.bin: %u", start);
    if((s = iso_block_to_name(disk, start))) {
      printf(", \"%s\"", s);
    }
    printf("\n");
  }

  printf(
    "  mbr partition table (chs %u/%u/%u%s):\n",
    disk->cylinders,
    disk->heads,
    disk->sectors,
    i ? "" : ", inconsistent geo"
  );

  for(i = 0; i < 4; i++) {
    print_ptable_entry(disk, i + 1, ptable + i);
  }

  pcnt = 5;

  link_count = 0;
  ext_base = 0;

  while((ptable_ext = find_ext_ptable(ptable, 4))) {
    if(!link_count++) {
      ext_base = ptable_ext->start.lin;
    }
    // arbitrary, but we don't want to loop forever
    if(link_count > 10000) {
      printf("too many partitions\n");
      break;
    }
    j = disk_read(disk, buf, ptable_ext->start.lin + ptable_ext->base, 1);
    if(j || read_word_le(buf + 0x1fe) != 0xaa55) {
      if(j) printf("disk read error - ");
      printf("not a valid extended partition\n");
      break;
    }
    parse_ptable(buf, 0x1be, ptable, ptable_ext->start.lin + ptable_ext->base, ext_base, 4);
    for(i = 0; i < 4; i++) {
      print_ptable_entry(disk, pcnt, ptable + i);
      if(ptable[i].valid && !is_ext_ptable(ptable + i)) pcnt++;
    }
  }
}


uint64_t dump_gpt_ptable(disk_t *disk, uint64_t addr)
{
  int i, j, name_len;
  unsigned char buf[disk->block_size];
  gpt_header_t *gpt;
  gpt_entry_t *p, *part, *part0;
  unsigned u, part_blocks;
  uint16_t *n;
  uint32_t orig_crc;
  uint64_t next_table = 0;
  uint64_t attr;

  if(!addr) return next_table;

  i = disk_read(disk, buf, addr, 1);

  gpt = (gpt_header_t *) buf;

  if(i || le64toh(gpt->signature) != EFI_MAGIC) {
    if(addr != 1) printf(SEP "\nno backup gpt\n");

    return next_table;
  }

  next_table = le64toh(gpt->backup_lba);

  orig_crc = gpt->header_crc;
  gpt->header_crc = 0;
  u = chksum_crc32(gpt, le32toh(gpt->header_size));
  gpt->header_crc = orig_crc;

  printf(SEP "\ngpt (%s) guid: %s\n",
    addr == 1 ? "primary" : "backup",
    efi_guid_decode(gpt->disk_guid)
  );
  printf("  sector size: %u\n", disk->block_size);
  if(opt.show.raw) printf("  header crc: 0x%08x\n", u);
  printf("  header: size %u, crc 0x%08x - %s\n",
    le32toh(gpt->header_size),
    le32toh(gpt->header_crc),
    le32toh(gpt->header_crc) == u ? "ok" : "wrong"
  );
  printf("  position: current %llu, backup %llu\n",
    (unsigned long long) le64toh(gpt->current_lba),
    (unsigned long long) le64toh(gpt->backup_lba)
  );
  printf("  usable area: %llu - %llu (size %lld)\n",
    (unsigned long long) le64toh(gpt->first_lba),
    (unsigned long long) le64toh(gpt->last_lba),
    (long long) (le64toh(gpt->last_lba) - le64toh(gpt->first_lba) + 1)
  );

  part_blocks = ((le32toh(gpt->partition_entries) * le32toh(gpt->partition_entry_size))
                + disk->block_size - 1) / disk->block_size;

  part = malloc(part_blocks * disk->block_size);

  if(!part_blocks || !part) return next_table;

  i = disk_read(disk, part, le64toh(gpt->partition_lba), part_blocks);

  if(i) {
    printf("error reading gpt\n");
    free(part);

    return next_table;
  }

  u = chksum_crc32(part, le32toh(gpt->partition_entries) * le32toh(gpt->partition_entry_size));

  if(opt.show.raw) printf("  partition table crc: 0x%08x\n", u);

  printf("  partition table: %llu - %llu (size %u, crc 0x%08x - %s), entries %u, entry_size %u\n",
    (unsigned long long) le64toh(gpt->partition_lba),
    (unsigned long long) (le64toh(gpt->partition_lba) + part_blocks - 1),
    part_blocks,
    le32toh(gpt->partition_crc),
    le32toh(gpt->partition_crc) == u ? "ok" : "wrong",
    le32toh(gpt->partition_entries),
    le32toh(gpt->partition_entry_size)
  );

  part0 = calloc(1, sizeof *part0);

  for(i = 0, p = part; i < le32toh(gpt->partition_entries); i++, p++) {
    if(!memcmp(p, part0, sizeof *part0)) continue;
    attr = le64toh(p->attributes);
    printf("  %-3d%c %llu - %llu (size %lld)\n",
      i + 1,
      (attr & 4) ? '*' : ' ',
      (unsigned long long) le64toh(p->first_lba),
      (unsigned long long) le64toh(p->last_lba),
      (long long) (le64toh(p->last_lba) - le64toh(p->first_lba) + 1)
    );
    printf("       type %s", efi_guid_decode(p->type_guid));
    char *s = efi_partition_type(efi_guid_decode(p->type_guid));
    if(s) printf(" (%s)", s);
    printf(", attributes 0x%llx", (unsigned long long) attr);
    if((attr & 7)) {
      char *pref = " (";
      if((attr & 1)) { printf("%ssystem", pref), pref = ", "; }
      if((attr & 2)) { printf("%shidden", pref), pref = ", "; }
      if((attr & 4)) { printf("%sboot", pref), pref = ", "; }
      printf(")");
    }
    printf("\n");
    printf("       guid %s\n", efi_guid_decode(p->partition_guid));

    name_len = (le32toh(gpt->partition_entry_size) - 56) / 2;
    n = p->name;
    for(j = name_len - 1; j > 0 && !n[j]; j--);
    name_len = n[j] ? j + 1 : j;
    
    printf("       name[%d] \"", name_len);
    n = p->name;
    for(j = 0; j < name_len; j++, n++) {
      // actually it's utf16le, but really...
      printf("%s", utf32_to_utf8(htole32(le16toh(*n))));
    }
    printf("\"\n");

    if(opt.show.raw) {
      printf("       name_hex[%d]", name_len);
      n = p->name;
      for(j = 0; j < name_len; j++, n++) {
        printf(" %04x", le16toh(*n));
      }
      printf("\n");
    }

    fs_detail(disk, 7, le64toh(p->first_lba));
  }

  free(part0);
  free(part);

  return next_table;
}


void dump_gpt_ptables(disk_t *disk)
{
  uint64_t u;

  for(disk->block_size = 0x200; disk->block_size <= 0x1000; disk->block_size <<= 1) {
    u = dump_gpt_ptable(disk, 1);
    if(!u) continue;
    dump_gpt_ptable(disk, u);
    return;
  }

  // printf("no primary gpt\n");
}


char *efi_partition_type(char *guid)
{
  static struct {
    char *guid;
    char *name;
  } types[] = {
    { "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "efi system" },
    { "0fc63daf-8483-4772-8e79-3d69d8477de4", "linux data" },
    { "e6d6d379-f507-44c2-a23c-238f2a3df928", "linux lvm" },
    { "a19d880f-05fc-4d3b-a006-743f0f84911e", "linux raid" },
    { "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "linux home" },
    { "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "linux swap" },
    { "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "windows data" },
    { "48465300-0000-11aa-aa11-00306543ecac", "hfs+" },
    { "21686148-6449-6e6f-744e-656564454649", "bios boot" },
    { "9e1a2d38-c612-4316-aa26-8b49521e5a8b", "prep" },
  };

  int i;

  for(i = 0; i < sizeof types / sizeof *types; i++) {
    if(!strcmp(types[i].guid, guid)) return types[i].name;

  }

  return NULL;
}


char *mbr_partition_type(unsigned id)
{
  static struct {
    unsigned id;
    char *name;
  } types[] = {
    { 0x00, "empty" },
    { 0x01, "fat12" },
    { 0x04, "fat16 <32mb" },
    { 0x05, "extended" },
    { 0x06, "fat16" },
    { 0x07, "ntfs" },
    { 0x0b, "fat32" },
    { 0x0c, "fat32 lba" },
    { 0x0e, "fat16 lba" },
    { 0x0f, "extended lba" },
    { 0x11, "fat12 hidden" },
    { 0x14, "fat16 <32mb hidden" },
    { 0x16, "fat16 hidden" },
    { 0x17, "ntfs hidden" },
    { 0x1b, "fat32 hidden" },
    { 0x1c, "fat32 lba hidden" },
    { 0x1e, "fat16 lba hidden" },
    { 0x41, "prep" },
    { 0x82, "swap" },
    { 0x83, "linux" },
    { 0x8e, "lvm" },
    { 0x96, "chrp iso9660" },
    { 0xde, "dell utility" },
    { 0xee, "gpt" },
    { 0xef, "efi" },
    { 0xfd, "linux raid" },
  };

  int i;

  for(i = 0; i < sizeof types / sizeof *types; i++) {
    if(types[i].id == id) return types[i].name;

  }

  return NULL;
}


void dump_apple_ptables(disk_t *disk)
{
  for(disk->block_size = 0x200; disk->block_size <= 0x1000; disk->block_size <<= 1) {
    if(dump_apple_ptable(disk)) return;
  }

  // printf("no apple partition table\n");
}


int dump_apple_ptable(disk_t *disk)
{
  int i, parts;
  unsigned u1, u2;
  unsigned char buf[disk->block_size];
  apple_entry_t *apple;
  char *s;

  i = disk_read(disk, buf, 1, 1);

  apple = (apple_entry_t *) buf;

  if(i || be16toh(apple->signature) != APPLE_MAGIC) return 0;

  parts = be32toh(apple->partitions);

  printf(SEP "\napple partition table: %d entries\n", parts);
  printf("  sector size: %d\n", disk->block_size);

  for(i = 1; i <= parts; i++) {
    if(disk_read(disk, buf, i, 1)) break;
    apple = (apple_entry_t *) buf;
    u1 = be32toh(apple->start);
    u2 = be32toh(apple->size);
    printf("%3d  %u - %llu (size %u)", i, u1, (unsigned long long) u1 + u2, u2);
    u1 = be32toh(apple->data_start);
    u2 = be32toh(apple->data_size);
    printf(", rel. %u - %llu (size %u)\n", u1, (unsigned long long) u1 + u2, u2);

    s = cname(apple->type, sizeof apple->type);
    printf("     type[%d] \"%s\"", (int) strlen(s), s);

    printf(", status 0x%x\n", be32toh(apple->status));

    s = cname(apple->name, sizeof apple->name);
    printf("     name[%d] \"%s\"\n", (int) strlen(s), s);

    fs_detail(disk, 5, be32toh(apple->start));
  }

  return 1;
}


// generate output as if we have 512 byte block size
// set to 0 or 2
#define BLK_FIX		2

void dump_eltorito(disk_t *disk)
{
  int i, j, sum;
  unsigned char buf[disk->block_size = 0x800];
  unsigned char zero[32];
  unsigned catalog;
  eltorito_t *el;
  char *s;
  static char *bt[] = {
    "no emulation", "1.2MB floppy", "1.44MB floppy", "2.88MB floppy", "hard disk", ""
  };

  memset(zero, 0, sizeof zero);

  i = disk_read(disk, buf, 0x10, 1);

  if(i || memcmp(buf, ISO_MAGIC, sizeof ISO_MAGIC - 1)) return;

  i = disk_read(disk, buf, 0x11, 1);

  if(i || memcmp(buf, ELTORITO_MAGIC, sizeof ELTORITO_MAGIC - 1)) {
    // printf("no el torito data\n");
    return;
  }

  catalog = le32toh(* (uint32_t *) (buf + 0x47));

  printf(SEP "\nel torito:\n");

  printf("  sector size: %d\n", disk->block_size >> BLK_FIX);

  printf("  boot catalog: %u\n", catalog << BLK_FIX);

  i = disk_read(disk, buf, catalog, 1);

  if(i) return;

  for(i = 0; i < disk->block_size/32; i++) {
    el = (eltorito_t *) (buf + 32 * i);
    if(!memcmp(buf + 32 * i, zero, 32)) continue;
    switch(el->any.header_id) {
      case 0x01:
        printf("  %-3d  type 0x%02x (validation entry)\n", i, el->validation.header_id);

        for(sum = j = 0; j < 16; j++) {
          sum += buf[32 * i + 2 * j] + (buf[32 * i + 2 * j + 1] << 8);
        }
        sum &= 0xffff;

        j = le16toh(el->validation.magic);

        printf("       platform id 0x%02x", el->validation.platform_id);
        printf(", crc 0x%04x (%s)", sum, sum ? "wrong" : "ok");
        printf(", magic %s\n", j == 0xaa55 ? "ok" : "wrong");

        s = cname(el->validation.name, sizeof el->validation.name);
        printf("       manufacturer[%d] \"%s\"\n", (int) strlen(s), s);

        break;

      case 0x44:
        printf("  %-3d  type 0x%02x (section entry extension)\n", i, el->any.header_id);
        printf("       info 0x%02x\n", el->extension.info);
        break;

      case 0x00:
      case 0x88:
        printf("  %-3d%c type 0x%02x (initial/default entry)\n", i, el->any.header_id ? '*' : ' ', el->any.header_id);
        s = bt[el->entry.media < 5 ? el->entry.media : 5];
        printf("       boot type %d (%s)\n", el->entry.media, s);
        printf("       load address 0x%05x", le16toh(el->entry.load_segment) << 4);
        printf(", system type 0x%02x\n", el->entry.system);
        printf("       start %d, size %d%s",
          le32toh(el->entry.start) << BLK_FIX,
          le16toh(el->entry.size),
          BLK_FIX ? "" : "/4"
        );
        if((s = iso_block_to_name(disk, le32toh(el->entry.start) << 2))) {
          printf(", \"%s\"", s);
        }
        s = cname(el->entry.name, sizeof el->entry.name);
        printf("\n       selection criteria 0x%02x \"%s\"\n", el->entry.criteria, s);
        fs_detail(disk, 7, le32toh(el->entry.start));
        break;

      case 0x90:
      case 0x91:
        printf(
          "  %-3d  type 0x%02x (%ssection header)\n",
          i,
          el->any.header_id,
          el->any.header_id == 0x91 ? "last " : ""
        );
        printf("       platform id 0x%02x", el->section.platform_id);
        s = cname(el->section.name, sizeof el->section.name);
        printf(", name[%d] \"%s\"\n", (int) strlen(s), s);
        printf("       entries %d\n", le16toh(el->section.entries));
        break;

      default:
        printf("  %-3d  type 0x%02x\n", i, el->any.header_id);
        break;
    }
  }
}
#undef BLK_FIX


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
