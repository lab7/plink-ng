#include "plink_common.h"

#include <ctype.h>
#include <time.h>
// no more mmap() dependency
// #include <fcntl.h>

#ifndef _WIN32
// #include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
  #include <sys/sysctl.h> // sysctl()
#endif

// #include <sys/stat.h>
#include <sys/types.h>

#define D_EPSILON 0.000244140625

static const char digit2_table[200] = {
  '0', '0', '0', '1', '0', '2', '0', '3', '0', '4',
  '0', '5', '0', '6', '0', '7', '0', '8', '0', '9',
  '1', '0', '1', '1', '1', '2', '1', '3', '1', '4',
  '1', '5', '1', '6', '1', '7', '1', '8', '1', '9',
  '2', '0', '2', '1', '2', '2', '2', '3', '2', '4',
  '2', '5', '2', '6', '2', '7', '2', '8', '2', '9',
  '3', '0', '3', '1', '3', '2', '3', '3', '3', '4',
  '3', '5', '3', '6', '3', '7', '3', '8', '3', '9',
  '4', '0', '4', '1', '4', '2', '4', '3', '4', '4',
  '4', '5', '4', '6', '4', '7', '4', '8', '4', '9',
  '5', '0', '5', '1', '5', '2', '5', '3', '5', '4',
  '5', '5', '5', '6', '5', '7', '5', '8', '5', '9',
  '6', '0', '6', '1', '6', '2', '6', '3', '6', '4',
  '6', '5', '6', '6', '6', '7', '6', '8', '6', '9',
  '7', '0', '7', '1', '7', '2', '7', '3', '7', '4',
  '7', '5', '7', '6', '7', '7', '7', '8', '7', '9',
  '8', '0', '8', '1', '8', '2', '8', '3', '8', '4',
  '8', '5', '8', '6', '8', '7', '8', '8', '8', '9',
  '9', '0', '9', '1', '9', '2', '9', '3', '9', '4',
  '9', '5', '9', '6', '9', '7', '9', '8', '9', '9'};

static inline char* uitoa_trunc4(uint32_t uii, char* start) {
  uint32_t quotient = uii / 100;
  memcpy(start, &(digit2_table[quotient * 2]), 2);
  uii -= 100 * quotient;
  if (uii) {
    start += 2;
    memcpy(start, &(digit2_table[uii * 2]), 2);
  }
  if (start[1] != '0') {
    return &(start[2]);
  }
  return &(start[1]);
}

char* div32768_print(uint32_t rawval, char* start) {
  *start++ = ' ';
  *start++ = '0' + (rawval >= 32768);
  rawval = rawval % 32768;
  if (!rawval) {
    return start;
  }
  *start++ = '.';
  // we wish to print (100000 * remainder + 16384) / 32768, banker's-rounded,
  // left-0-padded
  uint32_t five_decimal_places = (3125 * rawval + 512) / 1024;
  if ((rawval % 1024) == 512) {
    // banker's rounding is relevant for n/64 for n odd.  32768/64 = 512
    five_decimal_places &= ~1;
  }
  const uint32_t first_decimal_place = five_decimal_places / 10000;
  *start++ = '0' + first_decimal_place;
  const uint32_t last_four_digits = five_decimal_places - first_decimal_place * 10000;
  if (last_four_digits) {
    return uitoa_trunc4(last_four_digits, start);
  }
  return start;
}

int32_t bgen_to_gen(char* bgenname, char* out_genname, uint32_t snpid_chr) {
  unsigned char* bigstack_mark = g_bigstack_base;
  FILE* in_bgenfile = nullptr;
  FILE* out_genfile = nullptr;
  int32_t retval;
  {
    if (fopen_checked(out_genname, FOPEN_WB, &out_genfile)) {
      goto bgen_to_gen_ret_OPEN_FAIL;
    }

    if (fopen_checked(bgenname, FOPEN_RB, &in_bgenfile)) {
      goto bgen_to_gen_ret_OPEN_FAIL;
    }
    uint32_t initial_uints[5];
    if (!fread(initial_uints, 20, 1, in_bgenfile)) {
      goto bgen_to_gen_ret_READ_FAIL;
    }
    if (initial_uints[1] > initial_uints[0]) {
      logerrprint("Error: Invalid .bgen header.\n");
      goto bgen_to_gen_ret_INVALID_FORMAT;
    }
    const uint32_t raw_variant_ct = initial_uints[2];
    if (!raw_variant_ct) {
      logerrprint("Error: .bgen file contains no variants.\n");
      goto bgen_to_gen_ret_INVALID_FORMAT;
    }
    const uint32_t sample_ct = initial_uints[3];
    if (initial_uints[4] && (initial_uints[4] != 0x6e656762)) {
      logerrprint("Error: Invalid .bgen magic number.\n");
      goto bgen_to_gen_ret_INVALID_FORMAT;
    }
    if (fseeko(in_bgenfile, initial_uints[1], SEEK_SET)) {
      goto bgen_to_gen_ret_READ_FAIL;
    }
    uint32_t header_flags;
    if (!fread(&header_flags, 4, 1, in_bgenfile)) {
      goto bgen_to_gen_ret_READ_FAIL;
    }
    if (header_flags & (~5)) {
      header_flags = (header_flags >> 2) & 15;
      if (header_flags == 2) {
	logerrprint("Error: BGEN v1.2 input is not yet supported.  Use gen-convert or a similar tool\nto downcode to BGEN v1.1.\n");
      } else if (header_flags > 2) {
	logerrprint("Error: Unrecognized BGEN version.  Use gen-convert or a similar tool to\ndowncode to BGEN v1.1.\n");
      } else {
	logerrprint("Error: Unrecognized flags in .bgen header.\n");
      }
      goto bgen_to_gen_ret_INVALID_FORMAT;
    }

    // supports BGEN v1.1.
    uint16_t* bgen_probs = (uint16_t*)bigstack_alloc(6LU * sample_ct);
    if (!bgen_probs) {
      goto bgen_to_gen_ret_NOMEM;
    }
    char* writebuf;
    // three 5-decimal-place floating point values per sample
    if (bigstack_alloc_c(sample_ct * 24LU, &writebuf)) {
      goto bgen_to_gen_ret_NOMEM;
    }
    const uint32_t sample_ctx3 = sample_ct * 3;
    char* loadbuf = (char*)g_bigstack_base;
    uintptr_t loadbuf_size = bigstack_left();
    if (loadbuf_size > MAXLINEBUFLEN) {
      loadbuf_size = MAXLINEBUFLEN;
    } else if (loadbuf_size < 3 * 65536) {
      goto bgen_to_gen_ret_NOMEM;
    }
    if (fseeko(in_bgenfile, 4 + initial_uints[0], SEEK_SET)) {
      goto bgen_to_gen_ret_READ_FAIL;
    }
    const uint32_t bgen_compressed = header_flags & 1;
    const uint32_t bgen_multichar_alleles = (header_flags >> 2) & 1;
    if (!bgen_multichar_alleles) {
      logerrprint("BGEN v1.0 support is not implemented yet.\n");
      goto bgen_to_gen_ret_INVALID_FORMAT;
    }
    char numbuf[16];
    numbuf[0] = ' ';

    if (fopen_checked(out_genname, FOPEN_WB, &out_genfile)) {
      goto bgen_to_gen_ret_OPEN_FAIL;
    }
    for (uint32_t variant_uidx = 0; variant_uidx < raw_variant_ct; variant_uidx++) {
      uint32_t uii;
      if (!fread(&uii, 4, 1, in_bgenfile)) {
	goto bgen_to_gen_ret_READ_FAIL;
      }
      if (uii != sample_ct) {
	logerrprint("Error: Unexpected number of samples specified in SNP block header.\n");
	goto bgen_to_gen_ret_INVALID_FORMAT;
      }
      char* wptr;
      uint32_t alleles_are_identical;
      if (1) {
      // if (bgen_multichar_alleles) {
	// v1.1
	uint16_t snpid_slen;
	if (!fread(&snpid_slen, 2, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	char* rsid_start = loadbuf;
	if (!snpid_chr) {
	  if (fseeko(in_bgenfile, snpid_slen, SEEK_CUR)) {
	    goto bgen_to_gen_ret_READ_FAIL;
	  }
	} else {
	  if (!snpid_slen) {
	    logerrprint("Error: Length-0 SNP ID in .bgen file.\n");
	    goto bgen_to_gen_ret_INVALID_FORMAT;
	  }
	  if (!fread(loadbuf, snpid_slen, 1, in_bgenfile)) {
	    goto bgen_to_gen_ret_READ_FAIL;
	  }
	  loadbuf[snpid_slen] = '\0';
	  rsid_start = &(loadbuf[snpid_slen + 1]);
	}
	uint16_t rsid_slen;
	if (!fread(&rsid_slen, 2, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (!rsid_slen) {
	  logerrprint("Error: Length-0 rsID in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
	if (!fread(rsid_start, rsid_slen, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	char* chrom_name_start = &(rsid_start[rsid_slen]);
	uint16_t chrom_name_slen;
	if (fread(&chrom_name_slen, 1, 2, in_bgenfile) < 2) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (!snpid_chr) {
	  if (!chrom_name_slen) {
	    logerrprint("Error: Length-0 chromosome ID in .bgen file.\n");
	    goto bgen_to_gen_ret_INVALID_FORMAT;
	  }
	  if (!fread(chrom_name_start, chrom_name_slen, 1, in_bgenfile)) {
	    goto bgen_to_gen_ret_READ_FAIL;
	  }
	  if ((chrom_name_slen == 2) && (!memcmp(chrom_name_start, "NA", 2))) {
	    // convert 'NA' to 0
	    memcpy(chrom_name_start, "0", 2);
	  } else {
	    chrom_name_start[chrom_name_slen] = '\0';
	  }
	} else {
	  if (fseeko(in_bgenfile, chrom_name_slen, SEEK_CUR)) {
	    goto bgen_to_gen_ret_READ_FAIL;
	  }
	  chrom_name_start = loadbuf;
	}
	uint32_t bp_and_a1len[2];
	if (!fread(bp_and_a1len, 8, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (!bp_and_a1len[1]) {
	  logerrprint("Error: Length-0 allele ID in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
	// chrom_name_start (chromosome code) is already zero-terminated
	fputs(chrom_name_start, out_genfile);
	if (putc_checked(' ', out_genfile)) {
	  goto bgen_to_gen_ret_WRITE_FAIL;
	}
	fwrite(rsid_start, 1, rsid_slen, out_genfile);
	wptr = uint32toa_x(bp_and_a1len[0], ' ', &(numbuf[1]));
	fwrite(numbuf, 1, wptr - numbuf, out_genfile);

	// halve the limit since there are two alleles
	// (may want to enforce NON_BIGSTACK_MIN allele length limit?)
	if (bp_and_a1len[1] >= loadbuf_size / 2) {
	  if (loadbuf_size < MAXLINEBUFLEN) {
	    goto bgen_to_gen_ret_NOMEM;
	  }
	  logerrprint("Error: Excessively long allele in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
	if (!fread(loadbuf, bp_and_a1len[1], 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	loadbuf[bp_and_a1len[1]] = ' ';
	uint32_t a2len;
	if (!fread(&a2len, 4, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (a2len >= loadbuf_size / 2) {
	  if (loadbuf_size < MAXLINEBUFLEN) {
	    goto bgen_to_gen_ret_NOMEM;
	  }
	  logerrprint("Error: Excessively long allele in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
	char* a2_start = &(loadbuf[bp_and_a1len[1] + 1]);
	if (!fread(a2_start, a2len, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	alleles_are_identical = (a2len == bp_and_a1len[1]) && (!memcmp(loadbuf, a2_start, a2len));
	if (!alleles_are_identical) {
	  if (fwrite_checked(loadbuf, bp_and_a1len[1] + a2len + 1, out_genfile)) {
	    goto bgen_to_gen_ret_WRITE_FAIL;
	  }
	} else {
	  logerrprint("Error: A variant in the .bgen file has identical alleles.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	  /*
	  fputs("0 ", out_genfile);
	  if (fwrite_checked(a2_start, a2len, out_genfile)) {
	    goto bgen_to_gen_ret_WRITE_FAIL;
	  }
	  */
	}
	/*
      } else {
	// v1.0
	uii = 0;
	if (fread(&uii, 1, 1, in_bgenfile) < 1) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (fread(loadbuf, 1, 2 * uii + 9, in_bgenfile) < (2 * uii + 9)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	// save marker ID length since we might clobber it
	ukk = (unsigned char)(loadbuf[uii + 1]);
	int32_t cur_chrom_code;
	if (!snpid_chr) {
	  cur_chrom_code = ((unsigned char)(loadbuf[2 * uii + 2]));
	  if (cur_chrom_code > 24) {
	    if (cur_chrom_code == 255) {
	      // unknown
	      cur_chrom_code = 0;
	    } else if (cur_chrom_code > 252) {
	      // XY or MT
	      cur_chrom_code = cur_chrom_code - 228;
	    } else {
	      logerrprint("Error: Invalid chromosome code in BGEN v1.0 file.\n");
	      goto bgen_to_gen_ret_INVALID_FORMAT;
	    }
	  }
	  uint32toa_x((uint32_t)cur_chrom_code, '\0', loadbuf);
	  bufptr = loadbuf;
	} else {
	  ujj = (unsigned char)loadbuf[0];
	  bufptr = &(loadbuf[1]);
	  if ((ujj == 2) && (!memcmp(bufptr, "NA", 2))) {
	    *bufptr = '0';
	    ujj = 1;
	  }
	  bufptr[ujj] = '\0';
	  retval = get_or_add_chrom_code(bufptr, ".bgen file", 0, ujj, allow_extra_chroms, chrom_info_ptr, &cur_chrom_code);
	  if (retval) {
	    goto bgen_to_gen_ret_1;
	  }
	}
	if (!is_set(chrom_info_ptr->chrom_mask, cur_chrom_code)) {
	  if (bgen_compressed) {
	    if (fread(&uii, 1, 4, in_bgenfile) < 4) {
	      goto bgen_to_gen_ret_READ_FAIL;
	    }
	    if (fseeko(in_bgenfile, uii, SEEK_CUR)) {
	      goto bgen_to_gen_ret_READ_FAIL;
	    }
	  } else {
	    if (fseeko(in_bgenfile, ((uint64_t)sample_ct) * 6, SEEK_CUR)) {
	      goto bgen_to_gen_ret_READ_FAIL;
	    }
	  }
	  continue;
	}
	fputs(bufptr, outfile_bim);
	if (putc_checked(' ', outfile_bim)) {
	  goto bgen_to_gen_ret_WRITE_FAIL;
	}
	fwrite(&(loadbuf[uii + 2]), 1, ukk, outfile_bim);
	memcpy(&ujj, &(loadbuf[2 * uii + 3]), 4);
	bufptr = uint32toa_x(ujj, ' ', &(g_textbuf[3]));
	alleles_are_identical = (loadbuf[2 * uii + 7] == loadbuf[2 * uii + 8]);
	if (!alleles_are_identical) {
	  *bufptr++ = loadbuf[2 * uii + 7];
	} else {
	  *bufptr++ = '0';
	}
	*bufptr++ = ' ';
	*bufptr++ = loadbuf[2 * uii + 8];
	*bufptr++ = '\n';
	if (fwrite_checked(g_textbuf, bufptr - g_textbuf, outfile_bim)) {
	  goto bgen_to_gen_ret_WRITE_FAIL;
	}
	*/
      }
      if (bgen_compressed) {
	uint32_t compressed_block_blen;
	if (!fread(&compressed_block_blen, 4, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	if (compressed_block_blen > loadbuf_size) {
	  if (loadbuf_size < MAXLINEBUFLEN) {
	    goto bgen_to_gen_ret_NOMEM;
	  }
	  logerrprint("Error: Excessively long compressed SNP block in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
	if (!fread(loadbuf, compressed_block_blen, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
	uLongf zlib_ulongf = 6 * sample_ct;
	if (uncompress((Bytef*)bgen_probs, &zlib_ulongf, (Bytef*)loadbuf, compressed_block_blen) != Z_OK) {
	  logerrprint("Error: Invalid compressed SNP block in .bgen file.\n");
	  goto bgen_to_gen_ret_INVALID_FORMAT;
	}
      } else {
	if (!fread(bgen_probs, 6 * sample_ct, 1, in_bgenfile)) {
	  goto bgen_to_gen_ret_READ_FAIL;
	}
      }
      wptr = writebuf;
      for (uint32_t prob_idx = 0; prob_idx < sample_ctx3; ++prob_idx) {
	wptr = div32768_print(bgen_probs[prob_idx], wptr);
      }
      /*
      if (alleles_are_identical) {
	for (ulptr = writebuf; ulptr < (&(writebuf[sample_ctl2])); ulptr++) {
	  ulii = *ulptr;
	  *ulptr = ((~ulii) << 1) | ulii | FIVEMASK;
	}
	if (sample_ct % 4) {
	  writebuf[sample_ctl2 - 1] &= (ONELU << (2 * (sample_ct % BITCT2))) - ONELU;
	}
      }
      */
      *wptr++ = '\n';
      if (fwrite_checked(writebuf, wptr - writebuf, out_genfile)) {
	goto bgen_to_gen_ret_WRITE_FAIL;
      }
      if (!(variant_uidx % 1000)) {
	printf("\rbgen_to_gen: %uk variants converted.", variant_uidx / 1000);
	fflush(stdout);
      }
    }
    if (fclose_null(&in_bgenfile)) {
      goto bgen_to_gen_ret_READ_FAIL;
    }
    if (fclose_null(&out_genfile)) {
      goto bgen_to_gen_ret_WRITE_FAIL;
    }
    retval = 0;
    putc_unlocked('\r', stdout);
    LOGPRINTFWW("bgen_to_gen: %u variants written to %s .\n", raw_variant_ct, out_genname);
  }
  while (0) {
  bgen_to_gen_ret_NOMEM:
    retval = RET_NOMEM;
    break;
  bgen_to_gen_ret_OPEN_FAIL:
    retval = RET_OPEN_FAIL;
    break;
  bgen_to_gen_ret_READ_FAIL:
    retval = RET_READ_FAIL;
    break;
  bgen_to_gen_ret_WRITE_FAIL:
    retval = RET_WRITE_FAIL;
    break;
  bgen_to_gen_ret_INVALID_FORMAT:
    retval = RET_INVALID_FORMAT;
    break;
  }
  fclose_cond(in_bgenfile);
  fclose_cond(out_genfile);
  bigstack_reset(bigstack_mark);
  return retval;
}

int32_t init_logfile(uint32_t always_stderr, char* outname, char* outname_end) {
  memcpy(outname_end, ".log", 5);
  g_logfile = fopen(outname, "w");
  if (!g_logfile) {
    fflush(stdout);
    fprintf(stderr, "Error: Failed to open %s for logging.\n", outname);
    return RET_OPEN_FAIL;
  }
  fprintf(always_stderr? stderr : stdout, "Logging to %s.\n", outname);
  return 0;
}

void cleanup_logfile() {
  if (g_logfile) {
    if (!g_log_failed) {
      logstr("\nEnd time: ");
      time_t rawtime;
      time(&rawtime);
      logstr(ctime(&rawtime));
      if (fclose(g_logfile)) {
	fflush(stdout);
	fputs("Error: Failed to finish writing to log.\n", stderr);
      }
    } else {
      fclose(g_logfile);
    }
    g_logfile = nullptr;
  }
}

uintptr_t detect_mb() {
  int64_t llxx;
  // return zero if detection failed
  // see e.g. http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system .
#ifdef __APPLE__
  int32_t mib[2];
  mib[0] = CTL_HW;
  mib[1] = HW_MEMSIZE;
  llxx = 0;
  size_t sztmp = sizeof(int64_t);
  sysctl(mib, 2, &llxx, &sztmp, nullptr, 0);
  llxx /= 1048576;
#else
#ifdef _WIN32
  MEMORYSTATUSEX memstatus;
  memstatus.dwLength = sizeof(memstatus);
  GlobalMemoryStatusEx(&memstatus);
  llxx = memstatus.ullTotalPhys / 1048576;
#else
  llxx = ((uint64_t)sysconf(_SC_PHYS_PAGES)) * ((size_t)sysconf(_SC_PAGESIZE)) / 1048576;
#endif
#endif
  return llxx;
}

uintptr_t get_default_alloc_mb() {
  const uintptr_t total_mb = detect_mb();
  if (!total_mb) {
    return BIGSTACK_DEFAULT_MB;
  }
  if (total_mb < (BIGSTACK_MIN_MB * 2)) {
    return BIGSTACK_MIN_MB;
  }
  return (total_mb / 2);
}

int32_t init_bigstack(uintptr_t malloc_size_mb, uintptr_t* malloc_mb_final_ptr, unsigned char** bigstack_ua_ptr) {
  // guarantee contiguous malloc space outside of main workspace
  unsigned char* bubble = (unsigned char*)malloc(NON_BIGSTACK_MIN);
  if (!bubble) {
    return RET_NOMEM;
  }
  assert(malloc_size_mb >= BIGSTACK_MIN_MB);
#ifndef __LP64__
  assert(malloc_size_mb <= 2047);
#endif
  unsigned char* bigstack_ua = (unsigned char*)malloc(malloc_size_mb * 1048576 * sizeof(char));
  while (!bigstack_ua) {
    malloc_size_mb = (malloc_size_mb * 3) / 4;
    if (malloc_size_mb < BIGSTACK_MIN_MB) {
      malloc_size_mb = BIGSTACK_MIN_MB;
    }
    bigstack_ua = (unsigned char*)malloc(malloc_size_mb * 1048576 * sizeof(char));
    if ((!bigstack_ua) && (malloc_size_mb == BIGSTACK_MIN_MB)) {
      // switch to "goto cleanup" pattern if any more exit points are needed
      free(bubble);
      return RET_NOMEM;
    }
  }
  // force 64-byte align to make cache line sensitivity work
  unsigned char* bigstack_initial_base = (unsigned char*)round_up_pow2((uintptr_t)bigstack_ua, CACHELINE);
  g_bigstack_base = bigstack_initial_base;
  g_bigstack_end = &(bigstack_initial_base[(malloc_size_mb * 1048576 - (uintptr_t)(bigstack_initial_base - bigstack_ua)) & (~(CACHELINE - ONELU))]);
  free(bubble);
  *malloc_mb_final_ptr = malloc_size_mb;
  *bigstack_ua_ptr = bigstack_ua;
  return 0;
}

int32_t main(int32_t argc, char** argv) {
  unsigned char* bigstack_ua = nullptr;
  int32_t retval;
  {
    if (argc != 3) {
      fputs("Usage: bgen_to_gen [input .bgen] [output .gen]\n", stdout);
      goto main_ret_INVALID_CMDLINE;
    }
    char outname[16];
    char* outname_end = strcpya(outname, "bgen_to_gen");
    if (init_logfile(0, outname, outname_end)) {
      goto main_ret_OPEN_FAIL;
    }
    uintptr_t malloc_mb_final;
    if (init_bigstack(get_default_alloc_mb(), &malloc_mb_final, &bigstack_ua)) {
      goto main_ret_NOMEM;
    }
    retval = bgen_to_gen(argv[1], argv[2], 0);
    if (retval) {
      goto main_ret_1;
    }
  }
  while (0) {
  main_ret_NOMEM:
    retval = RET_NOMEM;
    break;
  main_ret_OPEN_FAIL:
    retval = RET_OPEN_FAIL;
    break;
  main_ret_INVALID_CMDLINE:
    retval = RET_INVALID_CMDLINE;
    break;
  }
 main_ret_1:
  cleanup_logfile();
  if (bigstack_ua) {
    free(bigstack_ua);
  }
  return retval;
}
