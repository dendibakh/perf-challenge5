#include "wordcount.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

// Assumptions
// 1. Function should read the input from the file, i.e. caching the input is
// not allowed.
// 2. The input is always encoded in UTF-8.
// 3. Break only on space, tab and newline (do not break on non-breaking space).
// 4. Sort words by frequency AND secondary sort in alphabetical order.

// Implementation rules
// 1. You can add new files but dependencies are generally not allowed unless it
// is a header-only library.
// 2. Your submission must be single-threaded, however feel free to implement
// multi-threaded version (optional).

#ifdef SOLUTION

#ifdef __linux__
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#undef min
#endif

// How many bytes to process at a time?
// This value isn't really configurable because we use u16
// as the type that has to hold indices into this length.
static constexpr usize CHUNK_SIZE = 64 * 1024;
// How many bytes to write() at a time?
static constexpr usize WRITE_CHUNK_SIZE = 32*1024;
// Should I output a histogram to stdout?
static constexpr bool OUTPUT_HISTOGRAM = false;
// Should I output timings to stderr?
static constexpr bool OUTPUT_TIMINGS = false;
// how large must a string be before it goes in hash tables instead of arrays?
static constexpr usize VERY_SHORT_STRING_LENGTH = 3;
// size of lookup table for very short string counts
static constexpr usize VERY_SHORT_COUNTS_LEN = (VERY_SHORT_STRING_LENGTH > 1) ? (1 << (8 * (VERY_SHORT_STRING_LENGTH - 1))) : 0;
// How large must a string be before it is excluded from the per-length string arrays in the indexer?
static constexpr usize MEDIUM_STRING_LENGTH = 256;

// Should I use MAP_POPULATE to pre-fault allocations?
// When using hugetlbfs, this makes the overall execution slower.
// However, if page faults introduce noise, it may be useful to
// use MAP_POPULATE when measuring the performance of various things.
static constexpr bool POPULATE_FILE_MEM = false;
static constexpr bool POPULATE_NON_FILE_MEM = false;
// Should I try to use hugepages?
static constexpr bool USE_HUGEPAGES = true;

// Should I bother inserting anything into hashtables?
static constexpr bool USE_HASHTABLES = true;

// How large shuld the hash table be? 23 => 8 million buckets.
static constexpr u64 SHORT_RHT_POW = 23;
// How large shuld the hash table be? 26 => 67 million buckets.
static constexpr u64 LONG_RHT_POW = 26;

// How few elements in a sort before we fall back to insertion sort?
static constexpr usize INSSORT_CUTOFF = 55;

struct __attribute__((packed)) Lenlo {
  u64 lenlo;
  inline __attribute__((always_inline)) u64 lo() const {
    return lenlo & 0xffffffffff;
  }
  inline __attribute__((always_inline)) u64 len() const {
    return lenlo >> 40;
  }
  inline __attribute__((always_inline)) bool operator< (const Lenlo &other) const {
    return lenlo < other.lenlo;
  }
};

struct __attribute__((packed)) RangeInBuffer {
  u16 lo;
  u16 hi;
};

struct __attribute__((packed)) CountCount {
  u32 count_per_string;
  u32 n_strings;
};


struct __attribute__((packed)) ShortHashString {
  u64 hash;
};

struct __attribute__((packed)) LongHashString {
  u64 hash;
  Lenlo lenlo;
  inline __attribute__((always_inline)) u64 lo() const {
    return lenlo.lo();
  }
  inline __attribute__((always_inline)) u64 len() const {
    return lenlo.len();
  }
};

struct __attribute__((packed)) ShortHashTable {
  static constexpr u64 RHT_POW = SHORT_RHT_POW;
  static constexpr u64 RHT_SHIFT = 64 - RHT_POW;
  static constexpr u64 RHT_LEN = 1 << RHT_POW;
  static constexpr u64 RHT_LEN_EXTENDED = RHT_LEN * 11 / 10;
  static constexpr u64 RHT_MASK = RHT_LEN - 1;
  Entry* xs;
  inline __attribute__((always_inline)) void prefetch(u64 hash) {
    __builtin_prefetch(xs + (hash >> RHT_SHIFT), 0, 0);
  }
  inline __attribute__((always_inline)) void insertRobberyVictim(Entry& tmp, u64& home_bucknum, u64& bucknum) {
    while (true) {
      if (xs[bucknum].getCount() == 0) {
        xs[bucknum] = tmp;
        return;
      }
      const u64 this_guys_home_bucknum = xs[bucknum].hash >> RHT_SHIFT;
      if (this_guys_home_bucknum > home_bucknum) {
        const Entry tmp_b = xs[bucknum];
        xs[bucknum] = tmp;
        tmp = tmp_b;
        home_bucknum = this_guys_home_bucknum;
      }
      bucknum++;
    }
  }
  inline __attribute__((always_inline)) void insertHash(const u64 hash) {
    u64 home_bucknum = hash >> RHT_SHIFT;
    u64 bucknum = hash >> RHT_SHIFT;
    while (true) {
      if (xs[bucknum].hash == hash) {
        xs[bucknum].setCount(xs[bucknum].getCount() + 1);
        return;
      }
      if (xs[bucknum].getCount() == 0) {
        xs[bucknum].setCount(1);
        xs[bucknum].hash = hash;
        return;
      }
      const u64 this_guys_home_bucknum = xs[bucknum].hash >> RHT_SHIFT;
      if (this_guys_home_bucknum > home_bucknum) {
        Entry tmp = xs[bucknum];
        xs[bucknum].setCount(1);
        xs[bucknum].hash = hash;
        home_bucknum = this_guys_home_bucknum;
        insertRobberyVictim(tmp, home_bucknum, bucknum);
        return;
      }
      bucknum++;
    }
  }
};

struct __attribute__((packed)) LongHashTable {
  static constexpr u64 RHT_POW = LONG_RHT_POW;
  static constexpr u64 RHT_SHIFT = 64 - RHT_POW;
  static constexpr u64 RHT_LEN = 1 << RHT_POW;
  static constexpr u64 RHT_LEN_EXTENDED = RHT_LEN * 11 / 10;
  static constexpr u64 RHT_MASK = RHT_LEN - 1;
  Entry* xs;
  Lenlo* lenlos;
  inline __attribute__((always_inline)) void prefetch(u64 hash) {
    __builtin_prefetch(xs + (hash >> RHT_SHIFT), 0, 0);
  }
  inline __attribute__((always_inline)) void insertRobberyVictim(Entry& tmp, Lenlo& tmpll, u64& home_bucknum, u64& bucknum) {
    while (true) {
      if (xs[bucknum].getCount() == 0) {
        xs[bucknum] = tmp;
        lenlos[bucknum] = tmpll;
        return;
      }
      const u64 this_guys_home_bucknum = xs[bucknum].hash >> RHT_SHIFT;
      if (this_guys_home_bucknum > home_bucknum) {
        const Entry tmp_b = xs[bucknum];
        xs[bucknum] = tmp;
        tmp = tmp_b;
        const Lenlo tmpll_b = lenlos[bucknum];
        lenlos[bucknum] = tmpll;
        tmpll = tmpll_b;
        home_bucknum = this_guys_home_bucknum;
      }
      bucknum = bucknum + 1;
    }
  }
  inline __attribute__((always_inline)) Entry* insertHash(const u64 hash, const Lenlo lenlo) {
    u64 home_bucknum = hash >> RHT_SHIFT;
    u64 bucknum = hash >> RHT_SHIFT;
    while (true) {
      if (xs[bucknum].hash == hash) {
        xs[bucknum].setCount(xs[bucknum].getCount() + 1);
        return &xs[bucknum];
      }
      if (xs[bucknum].getCount() == 0) {
        xs[bucknum].setCount(1);
        xs[bucknum].hash = hash;
        lenlos[bucknum] = lenlo;
        return &xs[bucknum];
      }
      const u64 this_guys_home_bucknum = xs[bucknum].hash >> RHT_SHIFT;
      if (this_guys_home_bucknum > home_bucknum) {
        Entry tmp = xs[bucknum];
        xs[bucknum].setCount(1);
        xs[bucknum].hash = hash;
        Lenlo tmpll = lenlos[bucknum];
        lenlos[bucknum] = lenlo;
        Entry* const ret = &xs[bucknum];
        home_bucknum = this_guys_home_bucknum;
        insertRobberyVictim(tmp, tmpll, home_bucknum, bucknum);
        return ret;
      }
      bucknum++;
    }
  }
};


#ifdef __linux__
int global_file;
usize global_mapped_len;
#elif defined(_WIN32)
HANDLE global_file;
HANDLE global_mapping;
#endif

u64 start_time;
u8* gpa;
u8* all_the_bytes = nullptr;
ShortHashTable short_rht;
LongHashTable long_rht;
u32* very_short_string_counts;
Lenlo* interblock_strings_base_ptr;
Lenlo* interblock_strings_ptr;
ShortHashString* short_hash_strings_base_ptr;
LongHashString* long_hash_strings_base_ptr;


u64 milliTimestamp() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

u8* alloc(usize size) {
  const usize rounded_size = (size + 8 - 1) & ~((usize)8 - 1);
  u8* const ret = gpa;
  gpa += rounded_size;
  return ret;
}

// I don't wanna use std.mem.lessThan!!
// Is this even a fast strcmp?
// I don't know.
// I read online that there's no point to using SIMD for this problem.
inline __attribute__((always_inline)) bool memLessThanLmao(const u8* a, const u8* b) {
  u64 in_a;
  u64 in_b;
  while (true) {
    memcpy(&in_a, a, 8);
    memcpy(&in_b, b, 8);
    if (in_a != in_b) {
      return __builtin_bswap64(in_a) < __builtin_bswap64(in_b);
    }
    a += 8;
    b += 8;
  }
}

// this is an in-place radix sort by length, which is only used to sort
// a list of ~75k lenlos.
// this runs in ~1ms so there's not much use caring about it.
template <int shift=56>
inline __attribute__((always_inline)) void radixSortByLength(Lenlo* array, Lenlo* end) {
  usize x;
  usize y;
  Lenlo value;
  Lenlo temp;
  u32 last[256] = {0};
  u32 pointer[256];

  for (Lenlo* ptr = array; ptr < end; ptr++) {
    last[(ptr->lenlo >> shift) & 0xFF] += 1;
  }

  pointer[0] = 0;
  for (x = 1; x < 256; x++) {
    pointer[x] = last[x-1];
    last[x] += last[x-1];
  }

  for (x = 0; x < 256; x++) {
    while (pointer[x] != last[x]) {
      value = array[pointer[x]];
      y = (value.lenlo >> shift) & 0xFF;
      while (x != y) {
        temp = array[pointer[y]];
        array[pointer[y]++] = value;
        value = temp;
        y = (value.lenlo >> shift) & 0xFF;
      }
      array[pointer[x]++] = value;
    }
  }

  if constexpr (shift > 40) {
    y = 0;
    for (x = 0; x < 256; x++) {
      const usize len = pointer[x] - y;
      if (len > 64) {
        radixSortByLength<shift-8>(array+y, array+pointer[x]);
      } else if (len > 1) {
        std::sort(array+y, array+pointer[x]);
      }
      y = pointer[x];
    }
  }
}

template<int idx, typename uret>
inline __attribute__((always_inline)) uret readTwoBytesOfCount(const u8* const bytes) {
  static_assert(idx == 0 || idx == 1 || idx == 2 || idx == 3, "oh dear");
  if constexpr (idx == 0) {
    return (uret)((((u16)bytes[3]) << 8) | bytes[2]);
  }
  if constexpr (idx == 2) {
    return (uret)((((u16)bytes[1]) << 8) | bytes[0]);
  }
  // because constexpr and template expansion is not as lazy as zig's comptime,
  // we can't static_assert everything we want to.
  // so we'll just abort if we got something invalid.
  // there's probably some way of making this be more lazy in C++, I just don't know it.
  abort();
  return 0;
}

template <int idx, typename uret>
inline __attribute__((always_inline)) uret readOneByteOfCount(const u8* const bytes) {
  static_assert(idx == 0 || idx == 1 || idx == 2 || idx == 3, "oh dear");
  if constexpr (idx == 2) {
    return (uret)bytes[1];
  }
  if constexpr (idx == 3) {
    return (uret)bytes[0];
  }
  abort();
  return 0;
}

template <int idx, typename uret>
inline __attribute__((always_inline)) uret readOneByte(const u8* const bytes) {
  static_assert(idx >= 0 && idx <= 7, "oh dear");
  return bytes[idx];
}

template <int idx, typename uret>
inline __attribute__((always_inline)) uret readTwoBytes(const u8* const bytes) {
  static_assert(idx >= 0 && idx <= 7, "oh dear");
  if constexpr ((idx & 1) == 0) {
    return (uret)((((u16)bytes[idx]) << 8) | bytes[idx+1]);
  }
  abort();
  return 0;
}

inline __attribute__((always_inline)) void loadMoreBytes(Entry* ptr, Entry* const end, const usize next_offset) {
  while (ptr < end) {
    (ptr++)->loadMoreBytes(all_the_bytes, next_offset);
  }
}

inline __attribute__((always_inline)) void insSort(Entry* const array, const usize len, const usize depth) {
  for (usize i = 1; i < len; i++) {
    const Entry tmp_ = array[i];
    const u64 icached = tmp_.getBytesBigEndian();
    const u8* const tmp = array[i].strptr(all_the_bytes) + depth;
    usize j = i;
    for (; j > 0; j--) {
      const u64 jcached = array[j-1].getBytesBigEndian();
      if (jcached < icached || (jcached == icached && memLessThanLmao(array[j-1].strptr(all_the_bytes) + depth, tmp))) {
        break;
      }
      array[j] = array[j-1];
    }
    array[j] = tmp_;
  }
}

inline __attribute__((always_inline)) void insSortIntoOtherArray(Entry* const src, Entry* const dst, const usize len, const usize depth) {
  dst[0] = src[0];
  for (usize i = 1; i < len; i++) {
    const u64 icached = src[i].getBytesBigEndian();
    const u8* const tmp = src[i].strptr(all_the_bytes) + depth;
    usize j = i;
    for (; j > 0; j--) {
      const u64 jcached = dst[j-1].getBytesBigEndian();
      if (jcached < icached || (jcached == icached && memLessThanLmao(dst[j-1].strptr(all_the_bytes) + depth, tmp))) {
        break;
      }
      dst[j] = dst[j-1];
    }
    dst[j] = src[i];
  }
}


template <int idx, bool array_is_final_destination, int BYTES_PER_LEVEL, typename ubucket, typename udigit>
void radixSortByBytesAdaptive(
  Entry* const array,
  Entry* const scratch,
  usize const arr_len,
  ubucket* const pointer_arrs,
  usize const next_offset
) {
  static constexpr usize buckets_len = (BYTES_PER_LEVEL == 2) ? 0x10000 : 0x100;
  static constexpr auto readBucket = (BYTES_PER_LEVEL == 2) ? readTwoBytes<idx, udigit> : readOneByte<idx, udigit>;
  ubucket* const bucketsize = pointer_arrs;
  ubucket* const bucketindex = pointer_arrs + buckets_len;

  for (usize i = 0; i < buckets_len; i++) {
    bucketsize[i] = 0;
  }

  for (usize i = 0; i < arr_len; i++) {
    const ubucket bucket = readBucket(&array[i].bytes[0]);
    bucketsize[bucket]++;
  }

  bucketindex[0] = 0;
  for (usize i = 1; i < buckets_len; i++) {
    bucketindex[i] = bucketindex[i-1] + bucketsize[i-1];
  }

  for (usize i = 0; i < arr_len; i++) {
    const ubucket bucket = readBucket(&array[i].bytes[0]);
    scratch[bucketindex[bucket]++] = array[i];
  }

  static constexpr bool NEED_MORE_BYTES = (idx + BYTES_PER_LEVEL) >= 8;
  const usize next_next_offset = NEED_MORE_BYTES ? next_offset + 8 : next_offset;
  static constexpr int next_idx = NEED_MORE_BYTES ? 0 : idx + BYTES_PER_LEVEL;

  usize lo = 0;
  for (usize i = 0; i < buckets_len; i++) {
    const usize len = bucketsize[i];
    const usize hi = lo + len;
    if (NEED_MORE_BYTES && len > 1) {
      loadMoreBytes(scratch+lo, scratch+hi, next_offset);
    }
    if (BYTES_PER_LEVEL == 2 && len >= 0x10000) {
      radixSortByBytesAdaptive<next_idx, !array_is_final_destination, 2, u32, u16>(
        scratch+lo, array+lo, len, (u32* const)bucketindex, next_next_offset);
    } else if (len > INSSORT_CUTOFF) {
      radixSortByBytesAdaptive<next_idx, !array_is_final_destination, 1, u16, u8>(
        scratch+lo, array+lo, len, (u16* const)bucketindex, next_next_offset);
    } else if (len > 1) {
      if (array_is_final_destination) {
        insSortIntoOtherArray(scratch+lo, array+lo, len, next_next_offset);
      } else {
        insSort(scratch+lo, len, next_next_offset);
      }
    } else if (len == 1 && array_is_final_destination) {
      array[lo] = scratch[lo];
    }
    lo = hi;
  }
}

template <int idx=0, bool array_is_final_destination=true, int BYTES_PER_LEVEL=2, typename ubucket=u32, typename udigit=u16>
void radixSortByCountAdaptive(
  Entry* const array,
  Entry* const scratch,
  usize const arr_len,
  ubucket* const pointer_arrs,
  CountCount*& countcounts
) {
  static constexpr usize buckets_len = (BYTES_PER_LEVEL == 2) ? 0x10000 : 0x100;
  static constexpr auto readBucket = (BYTES_PER_LEVEL == 2) ? readTwoBytesOfCount<idx, udigit> : readOneByteOfCount<idx, udigit>;
  ubucket* const bucketsize = pointer_arrs;
  ubucket* const bucketindex = pointer_arrs + buckets_len;

  for (usize i = 0; i < buckets_len; i++) {
    bucketsize[i] = 0;
  }

  for (usize i = 0; i < arr_len; i++) {
    const ubucket bucket = readBucket(&array[i].bytes[0]);
    bucketsize[bucket]++;
  }

  bucketindex[buckets_len-1] = 0;
  for (usize i = buckets_len-1; i > 0; i--) {
    bucketindex[i-1] = bucketindex[i] + bucketsize[i];
  }

  for (usize i = 0; i < arr_len; i++) {
    const ubucket bucket = readBucket(&array[i].bytes[0]);
    scratch[bucketindex[bucket]++] = array[i];
  }

  static constexpr int next_idx = idx + BYTES_PER_LEVEL;
  usize lo = 0;
  for (usize i = buckets_len; i > 0;) {
    i--;
    const usize len = bucketsize[i];
    const usize hi = lo + len;
    if (BYTES_PER_LEVEL == 2 && len >= 0x10000) {
      if constexpr (next_idx < 4) {
        radixSortByCountAdaptive<next_idx, !array_is_final_destination, 2, u32, u16>(
          scratch+lo, array+lo, len, (u32* const)bucketindex, countcounts);
      } else {
        *countcounts++ = { scratch[lo].getCount(), (u32)len };
        radixSortByBytesAdaptive<next_idx, !array_is_final_destination, 2, u32, u16>(
          scratch+lo, array+lo, len, (u32* const)bucketindex, 4);
      }
    } else if (len > 1) {
      if constexpr (next_idx < 4) {
        radixSortByCountAdaptive<next_idx, !array_is_final_destination, 1, u16, u8>(
          scratch+lo, array+lo, len, (u16* const)bucketindex, countcounts);
      } else {
        *countcounts++ = { scratch[lo].getCount(), (u32)len };
        radixSortByBytesAdaptive<next_idx, !array_is_final_destination, 1, u16, u8>(
          scratch+lo, array+lo, len, (u16* const)bucketindex, 4);
      }
    } else if (len == 1) {
      *countcounts++ = { scratch[lo].getCount(), (u32)len };
      if constexpr (array_is_final_destination) {
        array[lo] = scratch[lo];
      }
    }
    lo = hi;
  }
}


// given a u64 bitmask of whitespace positions,
// record them in our index of whitespace positions.
// It's not clear if this index even needs to exist, here or in simdjson.
// both this project and that one might do better to just store the raw
// "whitespace location bitmasks" in an array and compute the other
// information later when it is needed.
//inline fn handleWhitespace(offset_: usize, bits_: usize, noalias space_idxs_: *[]u16) void {
inline __attribute__((always_inline)) u64 handleWhitespace(const u16 offset, u64 bits, u16* const space_idxs) {
  const u64 cnt = _mm_popcnt_u64(bits);

  // This batching approach is just cribbed from simdjson.
  // BS = BATCH_SIZE ok.
  static constexpr usize UNCONDITIONAL_BS = 9;
  static constexpr usize SECOND_BS = 6;
  {
    for (usize i = 0; i < UNCONDITIONAL_BS; i++) {
      space_idxs[i] = offset + (u16)__builtin_ctzll(bits);
      bits &= bits - 1;
    }
  }

  if (cnt > UNCONDITIONAL_BS) {
    for (usize i = UNCONDITIONAL_BS; i < UNCONDITIONAL_BS + SECOND_BS; i++) {
      space_idxs[i] = offset + (u16)__builtin_ctzll(bits);
      bits &= bits - 1;
    }
  }

  if (cnt > UNCONDITIONAL_BS + SECOND_BS) {
    for (usize i = UNCONDITIONAL_BS + SECOND_BS; i < cnt; i++) {
      space_idxs[i] = offset + (u16)__builtin_ctzll(bits);
      bits &= bits - 1;
    }
  }

  return cnt;
}

inline __attribute__((always_inline)) u64 hashu64(const u64 xd) {
  return xd * 0x517cc1b727220a95ull;
}

inline __attribute__((always_inline)) u64 unhashu64(const u64 xd) {
  return xd * 0x2040003d780970bdull;
}

#ifdef __linux__
bool getMemoryInner(usize page_size, int HUGETLB, usize file_size, int fd, u8*& file_bytes_ptr, usize& file_bytes_len) {
  const usize rounded_size = ((file_size + 2 * page_size) / page_size) * page_size;
  static constexpr usize non_file_size = 8 * 512 * 1024 * (usize)1024;
  //const usize rounded_to_64_size = ((file_size + 64) / 64) * 64;
  const usize rounded_to_64_size = ((file_size + 128) / 128) * 128;
  static constexpr int POPULATE_NON_FILE = POPULATE_NON_FILE_MEM ? MAP_POPULATE : 0;
  // create an anonymous mapping of rounded_size plus 4 gigs
  u8* mapping_ptr = (u8*)mmap(
    nullptr,
    rounded_size + non_file_size,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | HUGETLB | POPULATE_NON_FILE,
    -1,
    0);
  if (mapping_ptr == MAP_FAILED) {
    return false;
  }
  static constexpr int POPULATE_FILE = POPULATE_FILE_MEM ? MAP_POPULATE : 0;
  // map the file to the beginning of the mmap'd region
  u8* junk_ptr = (u8*)mmap(
    mapping_ptr,
    rounded_size,
    PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_FIXED | POPULATE_FILE,
    fd,
    0);
  if (junk_ptr == MAP_FAILED) {
    return false;
  }
  // use madvise to tell the kernel we will read the file sequentially
  madvise(mapping_ptr, rounded_size, MADV_SEQUENTIAL);
  file_bytes_ptr = mapping_ptr;
  file_bytes_len = rounded_to_64_size;
  // pad the file with space characters
  memset(file_bytes_ptr + file_size, ' ', rounded_to_64_size - file_size);
  all_the_bytes = mapping_ptr;
  // the rest of these bytes are mine!!
  gpa = mapping_ptr + rounded_size;
  global_mapped_len = rounded_size + non_file_size;
  return true;
}

void getMemory(const std::string& filePath, u8*& file_bytes_ptr, usize& file_bytes_len) {
  if (all_the_bytes != nullptr) {
    close(global_file);
    munmap(all_the_bytes, global_mapped_len);
  }
  // open the file
  int fd = open(filePath.c_str(), O_RDONLY);
  global_file = fd;
  // stat the file to get its size
  struct stat st;
  fstat(fd, &st);
  usize file_size = st.st_size;

  if constexpr (USE_HUGEPAGES) {
    if (getMemoryInner(2*1024*1024, MAP_HUGETLB, file_size, fd, file_bytes_ptr, file_bytes_len)) {
      return;
    } else {
      if constexpr (OUTPUT_TIMINGS) {
        std::cerr << "Failed to map huge pages" << std::endl;
      }
    }
  }
  getMemoryInner(4096, 0, file_size, fd, file_bytes_ptr, file_bytes_len);
}
#elif defined(_WIN32)
void getMemory(const std::string& filePath, u8*& file_bytes_ptr, usize& file_bytes_len) {
  if (all_the_bytes != nullptr) {
    VirtualFree(all_the_bytes, 0, MEM_RELEASE);
    CloseHandle(global_file);
    CloseHandle(global_mapping);
  }
  auto fd = CreateFileA(
    filePath.c_str(),
    GENERIC_READ,
    0,
    NULL,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN ,
    NULL);
  global_file = fd;
  //std::cerr << "qq " << fd << " " << GetLastError() << std::endl;
  LARGE_INTEGER file_size_;
  GetFileSizeEx(fd, &file_size_);
  u64 file_size = file_size_.QuadPart;
  const usize page_size = 65536;
  const usize rounded_size = ((file_size + 2 * page_size) / page_size) * page_size;
  const usize rounded_down_size = (file_size / page_size) * page_size;
  static constexpr usize non_file_size = 8 * 512 * 1024 * (usize)1024;
  const usize rounded_to_64_size = ((file_size + 128) / 128) * 128;
  // create an anonymous mapping of rounded_size plus 4 gigs
  u8* mapping_ptr = (u8*)VirtualAlloc(
    NULL,
    rounded_size + non_file_size,
    MEM_RESERVE | MEM_COMMIT,
    PAGE_READWRITE);
  //std::cerr << "A " << (usize)mapping_ptr << " " << GetLastError() << " " << file_size << std::endl;
  // read the whole file using ReadFile
  /*
  for (usize i = 0; i < file_size; i += 1024 * 1024 * 1024) {
    DWORD amt_read;
    ReadFile(fd, mapping_ptr + i,
      1024 * 1024 * 1024, &amt_read, NULL);
    std::cerr << "B " << amt_read << " " << GetLastError() << std::endl;
  }*/
  
  // make a file mapping using CreateFileMappingA
  HANDLE file_mapping = CreateFileMappingA(
    fd,
    NULL,
    PAGE_READONLY,
    0,
    0,
    NULL);
  global_mapping = file_mapping;
  //std::cerr << "B " << (usize)file_mapping << " " << GetLastError() << std::endl;
  // unmap mmap'd region with VirtualFree
  VirtualFree(mapping_ptr, 0, MEM_RELEASE);
  if (rounded_down_size > 0) {
    // map the file to the beginning of the mmap'd region using MapViewOfFileEx
    u8* junk_ptr = (u8*)MapViewOfFileEx(
      file_mapping,
      FILE_MAP_READ | FILE_MAP_COPY,
      0,
      0,
      rounded_down_size,
      mapping_ptr);
    //std::cerr << "C " << (usize)junk_ptr << " " << GetLastError() << std::endl;
  }
  // get back the rest of our anonymous mapping using VirtualAlloc
  u8* junk_ptr2 = (u8*)VirtualAlloc(
    mapping_ptr + rounded_down_size,
    rounded_size + non_file_size - rounded_down_size,
    MEM_COMMIT | MEM_RESERVE,
    PAGE_READWRITE);
  //std::cerr << "D " << (usize)junk_ptr2 << " "<<  GetLastError() << std::endl;
  // create another mapping for just the last 0-64k of the file
  u8* junk_ptr3 = (u8*)MapViewOfFileEx(
    file_mapping,
    FILE_MAP_READ | FILE_MAP_COPY,
    rounded_down_size >> 32,
    rounded_down_size & 0xffffffff,
    file_size - rounded_down_size,
    NULL);
  //std::cerr << "E " << (usize)junk_ptr3 << " " << GetLastError() << std::endl;
  // copy the last 0-64k of the file into the mmap'd region
  memcpy(mapping_ptr + rounded_down_size, junk_ptr3, file_size - rounded_down_size);
  /**/
  file_bytes_ptr = mapping_ptr;
  file_bytes_len = rounded_to_64_size;
  //std::cerr << "oh noes " << (usize)mapping_ptr << std::endl;
  //std::cerr << GetLastError() << std::endl;
  // pad the file with space characters
  memset(file_bytes_ptr + file_size, ' ', rounded_to_64_size - file_size);
  //std::cerr << "oh teh noes ";
  all_the_bytes = mapping_ptr;
  // the rest of these bytes are mine!!
  gpa = mapping_ptr + rounded_size;
}
#endif

WordCountArray finish();

WordCountArray solve(const std::string& filePath) {
  start_time = milliTimestamp();
  u8 *file_lo;
  usize file_bytes_len;
  getMemory(filePath, file_lo, file_bytes_len);
  short_rht.xs = (Entry*)alloc(sizeof(Entry) * ShortHashTable::RHT_LEN_EXTENDED);
  long_rht.xs = (Entry*)alloc(sizeof(Entry) * LongHashTable::RHT_LEN_EXTENDED * 2);
  long_rht.lenlos = (Lenlo*)alloc(sizeof(Lenlo) * LongHashTable::RHT_LEN_EXTENDED);
  if constexpr (VERY_SHORT_COUNTS_LEN > 0) {
    very_short_string_counts = (u32*)alloc(sizeof(u32) * VERY_SHORT_COUNTS_LEN);
  }

  u64 prev_string_ends_mask = 0;
  u64 prev_string_starts_mask = 0;
  u64 prev_whitespace = ~((u64)0);
  u16 prev_offset = 0;

  u16* strings_lmao_base_ptr[MEDIUM_STRING_LENGTH];
  u16* strings_lmao_ptr[MEDIUM_STRING_LENGTH];
  for (usize i = 0; i < MEDIUM_STRING_LENGTH; i++) {
    strings_lmao_base_ptr[i] = (u16*)alloc(sizeof(u16) * CHUNK_SIZE/2);
    strings_lmao_ptr[i] = strings_lmao_base_ptr[i];
  }
  RangeInBuffer* strings_lmao_9_base_ptr = (RangeInBuffer*)alloc(sizeof(RangeInBuffer) * CHUNK_SIZE/2);
  RangeInBuffer* strings_lmao_9_ptr = strings_lmao_9_base_ptr;
  short_hash_strings_base_ptr = (ShortHashString*)alloc(sizeof(ShortHashString) * CHUNK_SIZE * 2);
  ShortHashString* short_hash_strings_ptr = short_hash_strings_base_ptr;
  long_hash_strings_base_ptr = (LongHashString*)alloc(sizeof(LongHashString) * CHUNK_SIZE * 2);
  LongHashString* long_hash_strings_ptr = long_hash_strings_base_ptr;

  // hi these are strings that span 2 blocks.
  // let's ignore them until the very end.
  interblock_strings_base_ptr = (Lenlo*)alloc(sizeof(Lenlo) * 0x20000);
  interblock_strings_ptr = interblock_strings_base_ptr;

  u16* string_starts_base_ptr = (u16*)alloc(sizeof(u16) * (CHUNK_SIZE / 2 + 64));
  u16* string_starts_ptr = string_starts_base_ptr;
  u16* string_ends_base_ptr = (u16*)alloc(sizeof(u16) * (CHUNK_SIZE / 2 + 64));
  u16* string_ends_ptr = string_ends_base_ptr;
  usize interblock_string_lo = 0;

  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done allocating in " << (now - start_time) << "ms\n";
    start_time = now;
  }
  u8* file_hi = file_lo + file_bytes_len;

  const u8x32 whitespace_table = _mm256_setr_epi8(
    ' ', 0, 0, 0, 0, 0, 0, 0, 0, '\t', '\n', 0, 0, 0, 0, 0,
    ' ', 0, 0, 0, 0, 0, 0, 0, 0, '\t', '\n', 0, 0, 0, 0, 0);

  // iterate over the file 64kb at a time
  while (file_lo < file_hi) {
    const usize slice_len = std::min(CHUNK_SIZE, (usize)(file_hi - file_lo));
    u8* const slice_end = file_lo + slice_len;
    u8* const start = file_lo;
    u8* p = file_lo;
    while (p < slice_end) {
      const u8x32 chunk0 = _mm256_loadu_si256((u8x32*)p);
      const u8x32 chunk1 = _mm256_loadu_si256((u8x32*)(p + 32));
      const u8x32 wss0 = _mm256_shuffle_epi8(whitespace_table, chunk0);
      const u8x32 wss1 = _mm256_shuffle_epi8(whitespace_table, chunk1);
      const u8x32 eq0 = _mm256_cmpeq_epi8(chunk0, wss0);
      const u8x32 eq1 = _mm256_cmpeq_epi8(chunk1, wss1);
      const u64 whitespace = ((u64)(u32)_mm256_movemask_epi8(eq0)) | (((u64)_mm256_movemask_epi8(eq1)) << 32);
      string_starts_ptr += handleWhitespace(prev_offset, prev_string_starts_mask, string_starts_ptr);
      string_ends_ptr += handleWhitespace(prev_offset, prev_string_ends_mask, string_ends_ptr);
      prev_offset = (u16)(p - start);
      const u64 prev_prev_whitespace = prev_whitespace;
      prev_whitespace = whitespace;
      prev_string_ends_mask   = ( prev_whitespace) & ~((prev_whitespace << 1) | (prev_prev_whitespace >> 63));
      prev_string_starts_mask = (~prev_whitespace) &  ((prev_whitespace << 1) | (prev_prev_whitespace >> 63));

      p += 64;
    }
    string_starts_ptr += handleWhitespace(prev_offset, prev_string_starts_mask, string_starts_ptr);
    string_ends_ptr += handleWhitespace(prev_offset, prev_string_ends_mask, string_ends_ptr);
    prev_string_starts_mask = 0;
    prev_string_ends_mask = 0;
    prev_offset = 0;

    for (usize i = 0; i < MEDIUM_STRING_LENGTH; i++) {
      strings_lmao_ptr[i] = strings_lmao_base_ptr[i];
    }
    strings_lmao_9_ptr = strings_lmao_9_base_ptr;

    // actually handle the strings, I guess.
    u16* const string_starts_end_ptr = string_starts_ptr;
    u16* const string_ends_end_ptr = string_ends_ptr;
    string_starts_ptr = string_starts_base_ptr;
    string_ends_ptr = string_ends_base_ptr;
    short_hash_strings_ptr = short_hash_strings_base_ptr;
    long_hash_strings_ptr = long_hash_strings_base_ptr;
    // if there is an end and no start, or there is an end and it's shorter than the first start
    // then we should handle the string for which we have the end and not the start.
    if ((string_ends_end_ptr > string_ends_base_ptr) &&
          ((string_starts_end_ptr == string_starts_base_ptr) ||
            (string_starts_ptr[0] > string_ends_ptr[0]))) {
      const u64 interblock_string_hi = (*string_ends_ptr++) + (u64)(file_lo - all_the_bytes);
      const u64 interblock_string_len = interblock_string_hi - interblock_string_lo;
      const u64 lenlo = interblock_string_lo | (interblock_string_len << 40);
      *interblock_strings_ptr++ = { lenlo };
    }
    // for strings contained within this block, bucketize by length
    while (string_ends_ptr < string_ends_end_ptr) {
      const u16 hi = *string_ends_ptr++;
      const u16 lo = *string_starts_ptr++;
      const u16 len = hi - lo;
      if (len < MEDIUM_STRING_LENGTH) {
        *strings_lmao_ptr[len]++ = lo;
      } else {
        *strings_lmao_9_ptr++ = { lo, hi };
      }
    }
    // if there's a string running off the end of this bock, handle it.
    if (string_starts_ptr < string_starts_end_ptr) {
      interblock_string_lo = *string_starts_ptr + (u64)(file_lo - all_the_bytes);
    }

    // hash medium strings (length 9-255 apparently)
    for (usize i = 9; i < MEDIUM_STRING_LENGTH; i++) {
      const u64 len_shifted = i << 40;
      const u16* const end_ptr = strings_lmao_ptr[i];
      const u16* ptr = strings_lmao_base_ptr[i];
      while (ptr < end_ptr) {
        const u16 lo_ = *ptr++;
        const u8* const str = file_lo + lo_;
        const u64 lo = lo_ + (u64)(file_lo - all_the_bytes);
        *long_hash_strings_ptr++ = { wyhash(str, i, 0, _wyp), lo | len_shifted };
      }
    }

    // hash long strings (length 256+)
    const RangeInBuffer* const strings_lmao_9_end_ptr = strings_lmao_9_ptr;
    for (const RangeInBuffer* ptr = strings_lmao_9_base_ptr; ptr < strings_lmao_9_end_ptr;) {
      const RangeInBuffer range = *ptr++;
      const u8* const str = file_lo + range.lo;
      const u64 lo = range.lo + (u64)(file_lo - all_the_bytes);
      const u64 len = range.hi - range.lo;
      *long_hash_strings_ptr++ = { wyhash(str, len, 0, _wyp), lo | (len << 40) };
    }

    // insert very short strings
    if constexpr (1 < VERY_SHORT_STRING_LENGTH) {
      const u16* const end_ptr = strings_lmao_ptr[1];
      const u16* ptr = strings_lmao_base_ptr[1];
      while (ptr < end_ptr) {
        const u16 lo_ = *ptr++;
        very_short_string_counts[file_lo[lo_]] += 1;
      }
    }
    if constexpr (2 < VERY_SHORT_STRING_LENGTH) {
      const u16* const end_ptr = strings_lmao_ptr[2];
      const u16* ptr = strings_lmao_base_ptr[2];
      while (ptr < end_ptr) {
        const u16 lo_ = *ptr++;
        const u8* const str = file_lo + lo_;
        u16 bytes;
        memcpy(&bytes, str, 2);
        very_short_string_counts[bytes] += 1;
      }
    }
    // hash short strings (length 3-8)
    for (usize i = VERY_SHORT_STRING_LENGTH; i < 9; i++) {
      // 1 => 0xff
      // 2 => 0xffff
      // 3 => 0xffffff, etc.
      const u64 mask = (~(u64)0) >> (64 - i * 8);
      const u16* const end_ptr = strings_lmao_ptr[i];
      const u16* ptr = strings_lmao_base_ptr[i];
      while (ptr < end_ptr) {
        const u16 lo_ = *ptr++;
        const u8* const str = file_lo + lo_;
        u64 bytes;
        memcpy(&bytes, str, 8);
        bytes &= mask;
        *short_hash_strings_ptr++ = { hashu64(bytes) };
      }
    }

    file_lo = p;

    static constexpr usize PREFETCHES = 40;
    if constexpr (USE_HASHTABLES) {
      const ShortHashString* const short_str_end = short_hash_strings_ptr;
      const ShortHashString* ptr = short_hash_strings_base_ptr;
      for (; ptr + PREFETCHES < short_str_end; ptr++) {
        short_rht.prefetch(ptr[PREFETCHES].hash);
        short_rht.insertHash(ptr[0].hash);
      }
      usize i = 0;
      for (; ptr < short_str_end; ptr++, i++) {
        long_rht.prefetch(long_hash_strings_base_ptr[i].hash);
        short_rht.insertHash(ptr[0].hash);
      }
    }
    if constexpr (USE_HASHTABLES) {
      const LongHashString* const long_str_end = long_hash_strings_ptr;
      const LongHashString* ptr = long_hash_strings_base_ptr;
      for (; ptr + PREFETCHES < long_str_end; ptr++) {
        long_rht.prefetch(ptr[PREFETCHES].hash);
        Entry* const entry = long_rht.insertHash(ptr[0].hash, ptr[0].lenlo);
        if (entry->getCount() == 1) {
          entry->setPrefix(all_the_bytes + ptr[0].lo());
        }
      }
      for (; ptr < long_str_end; ptr++) {
        Entry* const entry = long_rht.insertHash(ptr[0].hash, ptr[0].lenlo);
        if (entry->getCount() == 1) {
          entry->setPrefix(all_the_bytes + ptr[0].lo());
        }
      }
    }

    string_starts_ptr = string_starts_base_ptr;
    string_ends_ptr = string_ends_base_ptr;
  }
  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done parsing in " << (now - start_time) << "ms " <<
      (interblock_strings_ptr - interblock_strings_base_ptr) << "\n";
    start_time = now;
  }
  return finish();
}

WordCountArray finish() {
  {
    *interblock_strings_ptr++ = { ~(u64)0 };
    radixSortByLength(interblock_strings_base_ptr, interblock_strings_ptr);
    if constexpr (OUTPUT_TIMINGS) {
      const usize now = milliTimestamp();
      std::cerr << "Done bucketizing leftovers in " << (now - start_time) << "ms\n";
      start_time = now;
    }
    Lenlo* ptr = interblock_strings_base_ptr;
    u64 lo = ptr->lo();
    u64 len = ptr->len();
    if constexpr (1 < VERY_SHORT_STRING_LENGTH) {
      while (len == 1) {
        very_short_string_counts[all_the_bytes[lo]] += 1;
        ptr++;
        lo = ptr->lo();
        len = ptr->len();
      }
    }
    if constexpr (2 < VERY_SHORT_STRING_LENGTH) {
      while (len == 2) {
        const u8* const str = all_the_bytes + lo;
        u16 bytes;
        memcpy(&bytes, str, 2);
        very_short_string_counts[bytes] += 1;
        ptr++;
        lo = ptr->lo();
        len = ptr->len();
      }
    }
    ShortHashString* short_str_p = short_hash_strings_base_ptr;
    LongHashString* long_str_p = long_hash_strings_base_ptr;
    for (usize target_len = VERY_SHORT_STRING_LENGTH; target_len < 9; target_len++) {
      const u64 mask = (~(u64)0) >> (64 - target_len * 8);
      while (len == target_len) {
        const u8* const str = all_the_bytes + lo;
        u64 bytes;
        memcpy(&bytes, str, 8);
        bytes &= mask;
        *short_str_p++ = { hashu64(bytes) };
        ptr++;
        lo = ptr->lo();
        len = ptr->len();
      }
    }
    while (len < 0xffffff) {
      const u8* const str = all_the_bytes + lo;
      *long_str_p++ = { wyhash(str, len, 0, _wyp), lo | (len << 40) };
      ptr++;
      lo = ptr->lo();
      len = ptr->len();
    }
    static constexpr usize PREFETCHES = 40;
    if constexpr (USE_HASHTABLES) {
      const ShortHashString* const short_str_max = short_str_p;
      short_str_p = short_hash_strings_base_ptr;
      for (; short_str_p + PREFETCHES < short_str_max; short_str_p++) {
        short_rht.prefetch(short_str_p[PREFETCHES].hash);
        short_rht.insertHash(short_str_p[0].hash);
      }
      usize i = 0;
      for (usize i = 0; short_str_p < short_str_max; short_str_p++, i++) {
        long_rht.prefetch(long_hash_strings_base_ptr[i].hash);
        short_rht.insertHash(short_str_p[0].hash);
      }
    }
    if constexpr (USE_HASHTABLES) {
      const LongHashString* const long_str_max = long_str_p;
      long_str_p = long_hash_strings_base_ptr;
      for (; long_str_p + PREFETCHES < long_str_max; long_str_p++) {
        long_rht.prefetch(long_str_p[PREFETCHES].hash);
        Entry* const entry = long_rht.insertHash(long_str_p[0].hash, long_str_p[0].lenlo);
        if (entry->getCount() == 1) {
          entry->setPrefix(all_the_bytes + long_str_p[0].lo());
        }
      }
      for (; long_str_p < long_str_max; long_str_p++) {
        Entry* const entry = long_rht.insertHash(long_str_p[0].hash, long_str_p[0].lenlo);
        if (entry->getCount() == 1) {
          entry->setPrefix(all_the_bytes + long_str_p[0].lo());
        }
      }
    }
    if constexpr (OUTPUT_TIMINGS) {
      const usize now = milliTimestamp();
      std::cerr << "Done leftovers in " << (now - start_time) << "ms\n";
      start_time = now;
    }
  }

  // the large hash table is now done being a hash table.
  // so let's replace these: |count|prefix|-----hash-----|
  //             with these: |count|prefix|----lenlo-----|
  // so it's an array of strings that we can use for a sort.
  //
  // this is pretty similar to the cache struct used by "rantala/msd_A"
  // which is 12 bytes like so |4 cached string bytes|8 pointer bytes|
  // except that we have 4 extra bytes of count and a lenlo instead of a pointer.
  // At the same time, let's compress the large hash table so that it only
  // contains nonempty slots.
  Entry* long_entry_p = long_rht.xs;
  Entry* long_read_p = long_entry_p;
  const Lenlo* long_lenlo_read_p = long_rht.lenlos;
  const Entry* const long_rht_entry_max = long_rht.xs + LongHashTable::RHT_LEN_EXTENDED;
  for (; long_read_p < long_rht_entry_max; long_read_p++, long_lenlo_read_p++) {
      long_entry_p[0] = long_read_p[0];
      long_entry_p[0].hash = long_lenlo_read_p[0].lenlo;
      long_entry_p += long_read_p[0].getCount() != 0;
  }
  if constexpr (OUTPUT_TIMINGS) {
      const usize now = milliTimestamp();
      std::cerr << "Done compressing long table in " << (now - start_time) << "ms\n";
      start_time = now;
  }

  // let's insert all the entries from the small hash table into the large hash table.
  Entry* short_entry_p = short_rht.xs;
  const Entry* const short_rht_entry_max = short_rht.xs + ShortHashTable::RHT_LEN_EXTENDED;
  // The thing here is that 8-byte strings are up against the subsequent count
  // so we would like to zero the byte following the 8-byte block.
  // This prevents us from getting wrong results much much later when
  // we are insertion sorting without caring about string lengths.
  for (; short_entry_p < short_rht_entry_max; short_entry_p++) {
    const usize use_this = short_entry_p[0].getCount() != 0;
    short_entry_p[0].hash = unhashu64(short_entry_p[0].hash);
    long_entry_p[0].setCount(short_entry_p[0].getCount());
    short_entry_p[0].setCount(0);
    const u8* const str = (u8*)&short_entry_p[0].hash;
    long_entry_p[0].setPrefix(str);
    const u64 lo = str - all_the_bytes;
    const u64 len = 8 - __builtin_clzll(short_entry_p[0].hash) / 8;
    long_entry_p[0].hash = lo | (len << 40);
    long_entry_p += use_this;
  }
  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done inserting short strings into long table in " << (now - start_time) << "ms\n";
    start_time = now;
  }
  // let's insert all the very short strings into the large hash table.
  if constexpr (1 < VERY_SHORT_STRING_LENGTH) {
    for (usize idx = 0; idx < 256; idx++) {
      if (very_short_string_counts[idx] != 0) {
        const u8* const str = (u8*)&very_short_string_counts[idx];
        const u64 lo = str - all_the_bytes;
        const u64 len = 1;
        long_entry_p[0].setCount(very_short_string_counts[idx]);
        very_short_string_counts[idx] = idx;
        long_entry_p[0].setPrefix(str);
        long_entry_p[0].hash = lo | (len << 40);
        long_entry_p += 1;
      }
    }
  }
  if constexpr (2 < VERY_SHORT_STRING_LENGTH) {
    for (usize idx = 256; idx < 65536; idx++) {
      if (very_short_string_counts[idx] != 0) {
        const u8* const str = (u8*)&very_short_string_counts[idx];
        const u64 lo = str - all_the_bytes;
        const u64 len = 2;
        long_entry_p[0].setCount(very_short_string_counts[idx]);
        very_short_string_counts[idx] = idx;
        long_entry_p[0].setPrefix(str);
        long_entry_p[0].hash = lo | (len << 40);
        long_entry_p += 1;
      }
    }
  }
  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done inserting very short strings into long table in " << (now - start_time) << "ms\n";
    start_time = now;
  }
  const usize n_entries = (long_entry_p - long_rht.xs);
  if constexpr (OUTPUT_TIMINGS) {
    std::cerr << n_entries << " entries in long table\n";
  }
  u8* const pointer_arrs = alloc(sizeof(u32) * 0x10000 * 1024);
  CountCount* countcounts_ptr = (CountCount*)alloc(sizeof(CountCount) * 1024 * 1024);
  CountCount* const countcounts_base_ptr = countcounts_ptr;
  radixSortByCountAdaptive(
    long_rht.xs, long_entry_p, n_entries,
    (u32*)pointer_arrs, countcounts_ptr);
  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done sort in " << (now - start_time) << "ms\n";
    start_time = now;
  }
  // I sort of think we're done now, since we can do a linear scan of
  // wordcounts and access each one in constant time by keeping a cursor into
  // the array of entries and a cursor into the array of countcounts.
  // however, the counts aren't *literally in the same struct* as the strings at this point.
  // if we wanted to immediately do random accesses on the sorted array (why?),
  // then we would need to use a binary search or restore all the counts.
  // So we'll restore all the counts, which takes <50ms anyway.
  //
  // Also, writing the code to avoid doing this work now and do it during validation
  // instead sounds annoying.
  Entry* eptr = long_rht.xs;
  for (CountCount* cc = countcounts_base_ptr; cc < countcounts_ptr; cc++) {
    for (Entry* const end = eptr + cc->n_strings; eptr < end; eptr++) {
      eptr->setCount(cc->count_per_string);
    }
  }
  if constexpr (OUTPUT_TIMINGS) {
    const usize now = milliTimestamp();
    std::cerr << "Done restoring counts in " << (now - start_time) << "ms\n";
    start_time = now;
  }
  if constexpr (OUTPUT_HISTOGRAM) {
    if (false) {
    //  for (long_rht.xs[0..n_entries]) |entry| {
    //    const count = entry.count;
    //    if (count != prev_count) {
    //      count_str = try std.fmt.bufPrint(buffer[0..], "\t{d}\n", .{count});
    //    }
    //    const str = (all_the_bytes.ptr + (entry.hash & 0xffffffffff))[0..entry.hash >> 40];
    //    try buf_writer.writeAll(str);
    //    try buf_writer.writeAll(count_str);
    //  }
    } else {
      //for (countcounts) |countcount| {
      for (CountCount* ptr = countcounts_base_ptr; ptr < countcounts_ptr; ptr++) {
        //try std.fmt.format(buf_writer, "there are {d} strings that occur {d} times.\n", .{countcount.n_strings, countcount.count_per_string});
        std::cerr << "there are " << ptr->n_strings << " strings that occur " << ptr->count_per_string << " times.\n";
      }
    }
    //try buf_writer_.flush();
    if constexpr (OUTPUT_TIMINGS) {
      const usize now = milliTimestamp();
      std::cerr << "Done output in " << (now - start_time) << "ms\n";
      start_time = now;
    }
  }
  return { long_rht.xs, long_entry_p, all_the_bytes };
}

WordCountArray wordcount(std::string filePath) {
  return solve(filePath);
}

#else
// Baseline solution.
// Do not change it - you can use for quickly checking speedups
// of your solution agains the baseline, see check_speedup.py
std::vector<WordCount> wordcount(std::string filePath) {
  std::unordered_map<std::string, int> m;
  m.max_load_factor(0.5);

  std::vector<WordCount> mvec;

  std::ifstream inFile{filePath};
  if (!inFile) {
    std::cerr << "Invalid input file: " << filePath << "\n";
    return mvec;
  }

  std::string s;
  while (inFile >> s)
    m[s]++;

  mvec.reserve(m.size());
  for (auto &p : m)
    mvec.emplace_back(WordCount{p.second, move(p.first)});

  std::sort(mvec.begin(), mvec.end(), std::greater<WordCount>());
  return mvec;
}
#endif
