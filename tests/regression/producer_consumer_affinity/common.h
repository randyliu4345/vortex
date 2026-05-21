#ifndef _PRODUCER_CONSUMER_AFFINITY_COMMON_H_
#define _PRODUCER_CONSUMER_AFFINITY_COMMON_H_

#include <stdint.h>

#define PC_ENTRY_PRODUCER       0x50435052u  /* 'PCPR' */
#define PC_ENTRY_DISPATCH       0x50434450u  /* 'PCDP' */
#define PC_ENTRY_CONSUMER_CHILD 0x50434343u  /* 'PCCC' */

typedef struct {
  uint32_t entry_kind;
  uint32_t core_id;
  uint32_t elem_begin;  /* inclusive */
  uint32_t elem_end;    /* exclusive */
  uint64_t data_addr;   /* float* */
  uint64_t verify_addr; /* float* */
  float scale;
} child_arg_t;

typedef struct {
  uint32_t entry_kind;          /* PC_ENTRY_PRODUCER or PC_ENTRY_DISPATCH */
  uint32_t total_elems;
  uint32_t coarse_page_log2;    /* e.g. 16 -> 64KB pages */
  uint32_t num_banks;
  uint32_t num_cores;
  uint32_t consumer_block_x;
  uint32_t consumer_affinity;   /* 0:any, 1:pin to matching core */
  uint64_t data_addr;
  uint64_t verify_addr;
  uint64_t child_pool_addr;
  uint64_t kernel_pc;
  uint32_t active_chunks;
  uint32_t page_elems;
} master_arg_t;

#endif /* _PRODUCER_CONSUMER_AFFINITY_COMMON_H_ */
