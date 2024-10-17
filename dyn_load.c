/*
 * Copyright (c) 1991-1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1997 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 2009-2022 Ivan Maidanski
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

#include "private/gc_priv.h"

/*
 * This is incredibly OS specific code for tracking down data sections in
 * dynamic libraries.  There appears to be no way of doing this quickly
 * without groveling through undocumented data structures.  We would argue
 * that this is a bug in the design of the dlopen interface.  THIS CODE
 * MAY BREAK IN FUTURE OS RELEASES.  If this matters to you, don't hesitate
 * to let your vendor know ...
 *
 * None of this is safe with dlclose and incremental collection.
 * But then not much of anything is safe in the presence of dlclose.
 */

/* BTL: avoid circular redefinition of dlopen if SOLARIS+THREADS defined */
#undef GC_MUST_RESTORE_REDEFINED_DLOPEN
#if defined(GC_PTHREADS) && !defined(GC_NO_DLOPEN) \
    && !defined(GC_NO_THREAD_REDIRECTS) && !defined(GC_USE_LD_WRAP)
/* To support threads in Solaris, gc.h interposes on dlopen by        */
/* defining "dlopen" to be "GC_dlopen", which is implemented below.   */
/* However, both GC_FirstDLOpenedLinkMap() and GC_dlopen() use the    */
/* real system dlopen() in their implementation. We first remove      */
/* gc.h's dlopen definition and restore it later, after GC_dlopen().  */
#  undef dlopen
#  define GC_MUST_RESTORE_REDEFINED_DLOPEN
#endif /* !GC_NO_DLOPEN */

#if defined(SOLARISDL) && defined(THREADS) && !defined(SOLARIS) \
    && !defined(CPPCHECK)
#  error Fix mutual exclusion with dlopen
#endif

/* A user-supplied routine (custom filter) that might be called to      */
/* determine whether a DSO really needs to be scanned by the GC.        */
/* 0 means no filter installed.  May be unused on some platforms.       */
/* FIXME: Add filter support for more platforms.                        */
STATIC GC_has_static_roots_func GC_has_static_roots = 0;

#ifdef ANY_MSWIN
/* We traverse the entire address space and register all segments     */
/* that could possibly have been written to.                          */
STATIC void
GC_cond_add_roots(ptr_t base, ptr_t limit)
{
#  ifdef THREADS
  ptr_t curr_base = base;
  ptr_t next_stack_lo, next_stack_hi;
#  else
  ptr_t stack_top;
#  endif

  GC_ASSERT(I_HOLD_LOCK());
  if (base == limit)
    return;
#  ifdef THREADS
  for (;;) {
    GC_get_next_stack(curr_base, limit, &next_stack_lo, &next_stack_hi);
    if (ADDR_GE(next_stack_lo, limit))
      break;
    if (ADDR_LT(curr_base, next_stack_lo))
      GC_add_roots_inner(curr_base, next_stack_lo, TRUE);
    curr_base = next_stack_hi;
  }
  if (ADDR_LT(curr_base, limit))
    GC_add_roots_inner(curr_base, limit, TRUE);
#  else
  stack_top
      = PTR_ALIGN_DOWN(GC_approx_sp(), GC_sysinfo.dwAllocationGranularity);
  if (ADDR_LT(stack_top, limit) && ADDR_LT(base, GC_stackbottom)) {
    /* Part of the stack; ignore it.      */
    return;
  }
  GC_add_roots_inner(base, limit, TRUE);
#  endif
}

#  ifdef DYNAMIC_LOADING
/* GC_register_main_static_data is not needed unless DYNAMIC_LOADING. */
GC_INNER GC_bool
GC_register_main_static_data(void)
{
#    if defined(MSWINCE) || defined(CYGWIN32)
  /* Do we need to separately register the main static data segment? */
  return FALSE;
#    else
  return GC_no_win32_dlls;
#    endif
}
#    define HAVE_REGISTER_MAIN_STATIC_DATA
#  endif /* DYNAMIC_LOADING */

#  ifdef DEBUG_VIRTUALQUERY
void
GC_dump_meminfo(MEMORY_BASIC_INFORMATION *buf)
{
  GC_printf("BaseAddress= 0x%lx, AllocationBase= 0x%lx,"
            " RegionSize= 0x%lx(%lu)\n",
            buf->BaseAddress, buf->AllocationBase, buf->RegionSize,
            buf->RegionSize);
  GC_printf("\tAllocationProtect= 0x%lx, State= 0x%lx, Protect= 0x%lx, "
            "Type= 0x%lx\n",
            buf->AllocationProtect, buf->State, buf->Protect, buf->Type);
}
#  endif /* DEBUG_VIRTUALQUERY */

#  if defined(MSWINCE) || defined(CYGWIN32)
/* FIXME: Should we really need to scan MEM_PRIVATE sections?       */
/* For now, we don't add MEM_PRIVATE sections to the data roots for */
/* WinCE because otherwise SEGV fault sometimes happens to occur in */
/* GC_mark_from() (and, even if we use WRAP_MARK_SOME, WinCE prints */
/* a "Data Abort" message to the debugging console).                */
/* To workaround that, use -DGC_REGISTER_MEM_PRIVATE.               */
#    define GC_wnt TRUE
#  endif

GC_INNER void
GC_register_dynamic_libraries(void)
{
  ptr_t p, base, limit;

  GC_ASSERT(I_HOLD_LOCK());
#  ifdef MSWIN32
  if (GC_no_win32_dlls)
    return;
#  endif
  p = (ptr_t)GC_sysinfo.lpMinimumApplicationAddress;
  base = limit = p;
  while (ADDR_LT(p, (ptr_t)GC_sysinfo.lpMaximumApplicationAddress)) {
    MEMORY_BASIC_INFORMATION buf;
    size_t result = VirtualQuery((LPVOID)p, &buf, sizeof(buf));

#  ifdef MSWINCE
    if (0 == result) {
      if (ADDR(p) > GC_WORD_MAX - GC_sysinfo.dwAllocationGranularity)
        break; /* overflow */
      /* Page is free; advance to the next possible allocation base. */
      p = PTR_ALIGN_UP(p + 1, GC_sysinfo.dwAllocationGranularity);
    } else
#  endif
    /* else */ {
      DWORD protect;

      if (result != sizeof(buf))
        ABORT("Weird VirtualQuery result");
      if (ADDR(p) > GC_WORD_MAX - buf.RegionSize)
        break; /* overflow */

      protect = buf.Protect;
      if (buf.State == MEM_COMMIT
          && (protect == PAGE_EXECUTE_READWRITE
              || protect == PAGE_EXECUTE_WRITECOPY || protect == PAGE_READWRITE
              || protect == PAGE_WRITECOPY)
          && (buf.Type == MEM_IMAGE
#  ifdef GC_REGISTER_MEM_PRIVATE
              || (protect == PAGE_READWRITE && buf.Type == MEM_PRIVATE)
#  else
              /* There is some evidence that we cannot always   */
              /* ignore MEM_PRIVATE sections under Windows ME   */
              /* and predecessors.  Hence we now also check for */
              /* that case.                                     */
              || (!GC_wnt && buf.Type == MEM_PRIVATE)
#  endif
                  )
          && !GC_is_heap_base(buf.AllocationBase)) {
#  ifdef DEBUG_VIRTUALQUERY
        GC_dump_meminfo(&buf);
#  endif
        if (p != limit) {
          GC_cond_add_roots(base, limit);
          base = p;
        }
        limit = p + buf.RegionSize;
      }
      p += buf.RegionSize;
    }
  }
  GC_cond_add_roots(base, limit);
}

#elif defined(DYNAMIC_LOADING) /* && !ANY_MSWIN */

#  if !(defined(CPPCHECK) || defined(AIX) || defined(DARWIN) || defined(DGUX) \
        || defined(IRIX5) || defined(HAIKU) || defined(HPUX) || defined(HURD) \
        || defined(NACL) || defined(SCO_ELF) || defined(SOLARISDL)            \
        || ((defined(ANY_BSD) || defined(LINUX)) && defined(__ELF__))         \
        || (defined(ALPHA) && defined(OSF1))                                  \
        || (defined(OPENBSD) && defined(M68K)))
# error We only know how to find data segments of dynamic libraries for above.
#    error Additional SVR4 variants might not be too hard to add.
#  endif

#  if defined(DARWIN) && !defined(USE_DYLD_TO_BIND) \
      && !defined(NO_DYLD_BIND_FULLY_IMAGE)
#    include <dlfcn.h>
#  endif

#  ifdef SOLARISDL
#    include <dlfcn.h>
#    include <link.h>
#    include <sys/elf.h>
#  endif

#  if defined(NETBSD)
#    include <dlfcn.h>
#    include <machine/elf_machdep.h>
#    include <sys/param.h>
#    define ELFSIZE ARCH_ELFSIZE
#  endif

#  if defined(OPENBSD)
#    include <sys/param.h>
#    if (OpenBSD >= 200519) && !defined(HAVE_DL_ITERATE_PHDR)
#      define HAVE_DL_ITERATE_PHDR
#    endif
#  endif /* OPENBSD */

#  if defined(DGUX) || defined(HURD) || defined(NACL) || defined(SCO_ELF) \
      || ((defined(ANY_BSD) || defined(LINUX)) && defined(__ELF__))
#    include <stddef.h>
#    if !defined(OPENBSD) && !defined(HOST_ANDROID)
/* OpenBSD does not have elf.h file; link.h below is sufficient.    */
/* Exclude Android because linker.h below includes its own version. */
#      include <elf.h>
#    endif
#    ifdef HOST_ANDROID
/* If you don't need the "dynamic loading" feature, you may build   */
/* the collector with -D IGNORE_DYNAMIC_LOADING.                    */
#      ifdef BIONIC_ELFDATA_REDEF_BUG
/* Workaround a problem in Bionic (as of Android 4.2) which has   */
/* mismatching ELF_DATA definitions in sys/exec_elf.h and         */
/* asm/elf.h included from linker.h file (similar to EM_ALPHA).   */
#        include <asm/elf.h>
#        include <linux/elf-em.h>
#        undef ELF_DATA
#        undef EM_ALPHA
#      endif
#      include <link.h>
#      if !defined(GC_DONT_DEFINE_LINK_MAP) && !(__ANDROID_API__ >= 21)
/* link_map and r_debug are defined in link.h of NDK r10+.        */
/* bionic/linker/linker.h defines them too but the header         */
/* itself is a C++ one starting from Android 4.3.                 */
struct link_map {
  uintptr_t l_addr;
  char *l_name;
  uintptr_t l_ld;
  struct link_map *l_next;
  struct link_map *l_prev;
};
struct r_debug {
  int32_t r_version;
  struct link_map *r_map;
  void (*r_brk)(void);
  int32_t r_state;
  uintptr_t r_ldbase;
};
#      endif
#    else
/* Workaround missing extern "C" around _DYNAMIC symbol in link.h   */
/* of some Linux hosts.                                             */
EXTERN_C_BEGIN
#      include <link.h>
EXTERN_C_END
#    endif
#  endif

/* Newer versions of GNU/Linux define this macro.  We define it         */
/* similarly for any ELF systems that do not.                           */
#  ifndef ElfW
#    if defined(FREEBSD)
#      if __ELF_WORD_SIZE == 32
#        define ElfW(type) Elf32_##type
#      else
#        define ElfW(type) Elf64_##type
#      endif
#    elif defined(NETBSD) || defined(OPENBSD)
#      if ELFSIZE == 32
#        define ElfW(type) Elf32_##type
#      elif ELFSIZE == 64
#        define ElfW(type) Elf64_##type
#      else
#        error Missing ELFSIZE define
#      endif
#    else
#      if !defined(ELF_CLASS) || ELF_CLASS == ELFCLASS32
#        define ElfW(type) Elf32_##type
#      else
#        define ElfW(type) Elf64_##type
#      endif
#    endif
#  endif

#  if defined(SOLARISDL) && !defined(USE_PROC_FOR_LIBRARIES)

EXTERN_C_BEGIN
extern ElfW(Dyn) _DYNAMIC;
EXTERN_C_END

STATIC struct link_map *
GC_FirstDLOpenedLinkMap(void)
{
  ElfW(Dyn) * dp;
  static struct link_map *cachedResult = 0;
  static ElfW(Dyn) *dynStructureAddr = 0;
  /* BTL: added to avoid Solaris 5.3 ld.so _DYNAMIC bug   */

#    ifdef SUNOS53_SHARED_LIB
  /* BTL: Avoid the Solaris 5.3 bug that _DYNAMIC isn't being set */
  /* up properly in dynamically linked .so's. This means we have  */
  /* to use its value in the set of original object files loaded  */
  /* at program startup.                                          */
  if (0 == dynStructureAddr) {
    void *startupSyms = dlopen(0, RTLD_LAZY);

    dynStructureAddr = (ElfW(Dyn) *)dlsym(startupSyms, "_DYNAMIC");
    /* Note: dlclose() is not called intentionally. */
  }
#    else
  dynStructureAddr = &_DYNAMIC;
#    endif

  if (0 == COVERT_DATAFLOW(ADDR(dynStructureAddr))) {
    /* _DYNAMIC symbol not resolved. */
    return NULL;
  }
  if (cachedResult == 0) {
    int tag;

    for (dp = (ElfW(Dyn) *)&_DYNAMIC; (tag = dp->d_tag) != 0; dp++) {
      if (tag == DT_DEBUG) {
        const struct r_debug *rd = (struct r_debug *)dp->d_un.d_ptr;
        if (rd != NULL) {
          const struct link_map *lm = rd->r_map;
          if (lm != NULL)
            cachedResult = lm->l_next; /* might be NULL */
        }
        break;
      }
    }
  }
  return cachedResult;
}

GC_INNER void
GC_register_dynamic_libraries(void)
{
  struct link_map *lm;

  GC_ASSERT(I_HOLD_LOCK());
  for (lm = GC_FirstDLOpenedLinkMap(); lm != 0; lm = lm->l_next) {
    ElfW(Ehdr) * e;
    ElfW(Phdr) * p;
    unsigned long offset;
    char *start;
    int i;

    e = (ElfW(Ehdr) *)lm->l_addr;
    p = ((ElfW(Phdr) *)(((char *)(e)) + e->e_phoff));
    offset = (unsigned long)lm->l_addr;
    for (i = 0; i < (int)e->e_phnum; i++, p++) {
      switch (p->p_type) {
      case PT_LOAD:
        if (!(p->p_flags & PF_W))
          break;
        start = (char *)p->p_vaddr + offset;
        GC_add_roots_inner(start, start + p->p_memsz, TRUE);
        break;
      default:
        break;
      }
    }
  }
}

#  endif /* SOLARISDL && !USE_PROC_FOR_LIBRARIES */

#  if defined(DGUX) || defined(HURD) || defined(NACL) || defined(SCO_ELF) \
      || ((defined(ANY_BSD) || defined(LINUX)) && defined(__ELF__))

#    ifdef USE_PROC_FOR_LIBRARIES

#      include <string.h>

#      include <fcntl.h>
#      include <sys/stat.h>

#      define MAPS_BUF_SIZE (32 * 1024)

/* Sort an array of HeapSects by start address.                         */
/* Unfortunately at least some versions of                              */
/* Linux qsort end up calling malloc by way of sysconf, and hence can't */
/* be used in the collector.  Hence we roll our own.  Should be         */
/* reasonably fast if the array is already mostly sorted, as we expect  */
/* it to be.                                                            */
static void
sort_heap_sects(struct HeapSect *base, size_t number_of_elements)
{
  signed_word n = (signed_word)number_of_elements;
  signed_word nsorted = 1;

  while (nsorted < n) {
    signed_word i;

    while (nsorted < n
           && ADDR_LT(base[nsorted - 1].hs_start, base[nsorted].hs_start)) {
      ++nsorted;
    }
    if (nsorted == n)
      break;
    GC_ASSERT(ADDR_LT(base[nsorted].hs_start, base[nsorted - 1].hs_start));
    for (i = nsorted - 1;
         i >= 0 && ADDR_LT(base[i + 1].hs_start, base[i].hs_start); --i) {
      struct HeapSect tmp = base[i];

      base[i] = base[i + 1];
      base[i + 1] = tmp;
    }
    GC_ASSERT(ADDR_LT(base[nsorted - 1].hs_start, base[nsorted].hs_start));
    ++nsorted;
  }
}

STATIC void
GC_register_map_entries(const char *maps)
{
  const char *prot, *path;
  ptr_t start, end;
  ptr_t least_ha, greatest_ha;
  unsigned maj_dev;
  unsigned i;

  GC_ASSERT(I_HOLD_LOCK());
  sort_heap_sects(GC_our_memory, GC_n_memory);
  least_ha = GC_our_memory[0].hs_start;
  greatest_ha = GC_our_memory[GC_n_memory - 1].hs_start
                + GC_our_memory[GC_n_memory - 1].hs_bytes;

  for (;;) {
    maps = GC_parse_map_entry(maps, &start, &end, &prot, &maj_dev, &path);
    if (NULL == maps)
      break;

    if (prot[1] == 'w') {
      /* This is a writable mapping.  Add it to           */
      /* the root set unless it is already otherwise      */
      /* accounted for.                                   */
#      ifndef THREADS
      if (ADDR_GE(GC_stackbottom, start) && ADDR_GE(end, GC_stackbottom)) {
        /* Stack mapping; discard it.   */
        continue;
      }
#      endif
#      if defined(E2K) && defined(__ptr64__)
      /* TODO: avoid hard-coded addresses */
      if (ADDR(start) == 0xc2fffffff000UL && ADDR(end) == 0xc30000000000UL
          && path[0] == '\n')
        continue; /* discard some special mapping */
#      endif
      if (path[0] == '[' && strncmp(path + 1, "heap]", 5) != 0)
        continue; /* discard if a pseudo-path unless "[heap]" */

#      ifdef THREADS
      /* This may fail, since a thread may already be           */
      /* unregistered, but its thread stack may still be there. */
      /* That can fail because the stack may disappear while    */
      /* we're marking.  Thus the marker is, and has to be      */
      /* prepared to recover from segmentation faults.          */

      if (GC_segment_is_thread_stack(start, end))
        continue;

        /* FIXME: NPTL squirrels                                  */
        /* away pointers in pieces of the stack segment that we   */
        /* don't scan.  We work around this                       */
        /* by treating anything allocated by libpthread as        */
        /* uncollectible, as we do in some other cases.           */
        /* A specifically identified problem is that              */
        /* thread stacks contain pointers to dynamic thread       */
        /* vectors, which may be reused due to thread caching.    */
        /* They may not be marked if the thread is still live.    */
        /* This specific instance should be addressed by          */
        /* INCLUDE_LINUX_THREAD_DESCR, but that doesn't quite     */
        /* seem to suffice.                                       */
        /* We currently trace entire thread stacks, if they are   */
        /* are currently cached but unused.  This is              */
        /* very suboptimal for performance reasons.               */
#      endif
      /* We no longer exclude the main data segment.              */
      if (ADDR_GE(least_ha, end) || ADDR_GE(start, greatest_ha)) {
        /* The easy case; just trace the entire segment.  */
        GC_add_roots_inner(start, end, TRUE);
        continue;
      }
      /* Add sections that don't belong to us. */
      i = 0;
      while (ADDR_LT(GC_our_memory[i].hs_start + GC_our_memory[i].hs_bytes,
                     start)) {
        ++i;
      }
      GC_ASSERT(i < GC_n_memory);
      if (ADDR_GE(start, GC_our_memory[i].hs_start)) {
        start = GC_our_memory[i].hs_start + GC_our_memory[i].hs_bytes;
        ++i;
      }
      for (; i < GC_n_memory && ADDR_LT(start, end)
             && ADDR_LT(GC_our_memory[i].hs_start, end);
           ++i) {
        if (ADDR_LT(start, GC_our_memory[i].hs_start))
          GC_add_roots_inner(start, GC_our_memory[i].hs_start, TRUE);
        start = GC_our_memory[i].hs_start + GC_our_memory[i].hs_bytes;
      }
      if (ADDR_LT(start, end))
        GC_add_roots_inner(start, end, TRUE);
    } else if (prot[0] == '-' && prot[1] == '-' && prot[2] == '-') {
      /* Even roots added statically might disappear partially    */
      /* (e.g. the roots added by INCLUDE_LINUX_THREAD_DESCR).    */
      GC_remove_roots_subregion(start, end);
    }
  }
}

GC_INNER void
GC_register_dynamic_libraries(void)
{
  GC_register_map_entries(GC_get_maps());
}

/* We now take care of the main data segment ourselves: */
GC_INNER GC_bool
GC_register_main_static_data(void)
{
  return FALSE;
}

#      define HAVE_REGISTER_MAIN_STATIC_DATA

#    else /* !USE_PROC_FOR_LIBRARIES */

/* The following is the preferred way to walk dynamic libraries */
/* for glibc 2.2.4+.  Unfortunately, it doesn't work for older  */
/* versions.  Thanks to Jakub Jelinek for most of the code.     */

#      if GC_GLIBC_PREREQ(2, 3) || defined(HOST_ANDROID)
/* Are others OK here, too? */
#        ifndef HAVE_DL_ITERATE_PHDR
#          define HAVE_DL_ITERATE_PHDR
#        endif
#        ifdef HOST_ANDROID
/* Android headers might have no such definition for some targets.  */
EXTERN_C_BEGIN
extern int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
                           void *data);
EXTERN_C_END
#        endif
#      endif /* __GLIBC__ >= 2 || HOST_ANDROID */

#      if defined(__DragonFly__) || defined(__FreeBSD_kernel__) \
          || (defined(FREEBSD) && __FreeBSD__ >= 7)
/* On the FreeBSD system, any target system at major version 7 shall   */
/* have dl_iterate_phdr; therefore, we need not make it weak as below. */
#        ifndef HAVE_DL_ITERATE_PHDR
#          define HAVE_DL_ITERATE_PHDR
#        endif
#        define DL_ITERATE_PHDR_STRONG
#      elif defined(HAVE_DL_ITERATE_PHDR)
/* We have the header files for a glibc that includes dl_iterate_phdr.*/
/* It may still not be available in the library on the target system. */
/* Thus we also treat it as a weak symbol.                            */
EXTERN_C_BEGIN
#        pragma weak dl_iterate_phdr
EXTERN_C_END
#      endif

#      if defined(HAVE_DL_ITERATE_PHDR)

#        ifdef PT_GNU_RELRO
/* Instead of registering PT_LOAD sections directly, we keep them       */
/* in a temporary list, and filter them by excluding PT_GNU_RELRO       */
/* segments.  Processing PT_GNU_RELRO sections with                     */
/* GC_exclude_static_roots instead would be superficially cleaner.  But */
/* it runs into trouble if a client registers an overlapping segment,   */
/* which unfortunately seems quite possible.                            */

#          define MAX_LOAD_SEGS MAX_ROOT_SETS

static struct load_segment {
  ptr_t start;
  ptr_t end;
  /* Room for a second segment if we remove a RELRO segment */
  /* from the middle.                                       */
  ptr_t start2;
  ptr_t end2;
} load_segs[MAX_LOAD_SEGS];

static int n_load_segs;
static GC_bool load_segs_overflow;
#        endif /* PT_GNU_RELRO */

STATIC int
GC_register_dynlib_callback(struct dl_phdr_info *info, size_t size, void *ptr)
{
  const ElfW(Phdr) * p;
  ptr_t start, end;
  int i;

  GC_ASSERT(I_HOLD_LOCK());
  /* Make sure struct dl_phdr_info is at least as big as we need.  */
  if (size
      < offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum))
    return -1;

  p = info->dlpi_phdr;
  for (i = 0; i < (int)info->dlpi_phnum; i++, p++) {
    if (p->p_type == PT_LOAD) {
      GC_has_static_roots_func callback = GC_has_static_roots;
      if ((p->p_flags & PF_W) == 0)
        continue;

      start = (ptr_t)p->p_vaddr + info->dlpi_addr;
      end = start + p->p_memsz;
      if (callback != 0 && !callback(info->dlpi_name, start, p->p_memsz))
        continue;
#        ifdef PT_GNU_RELRO
#          if CPP_PTRSZ >= 64
      /* TODO: GC_push_all eventually does the correct          */
      /* rounding to the next multiple of ALIGNMENT, so, most   */
      /* probably, we should remove the corresponding assertion */
      /* check in GC_add_roots_inner along with this code line. */
      /* start pointer value may require aligning.              */
      start = PTR_ALIGN_DOWN(start, sizeof(ptr_t));
#          endif
      if (n_load_segs >= MAX_LOAD_SEGS) {
        if (!load_segs_overflow) {
          WARN("Too many PT_LOAD segments;"
               " registering as roots directly...\n",
               0);
          load_segs_overflow = TRUE;
        }
        GC_add_roots_inner(start, end, TRUE);
      } else {
        load_segs[n_load_segs].start = start;
        load_segs[n_load_segs].end = end;
        load_segs[n_load_segs].start2 = NULL;
        load_segs[n_load_segs].end2 = NULL;
        ++n_load_segs;
      }
#        else
      GC_add_roots_inner(start, end, TRUE);
#        endif /* !PT_GNU_RELRO */
    }
  }

#        ifdef PT_GNU_RELRO
  p = info->dlpi_phdr;
  for (i = 0; i < (int)info->dlpi_phnum; i++, p++) {
    if (p->p_type == PT_GNU_RELRO) {
      /* This entry is known to be constant and will eventually be    */
      /* remapped as read-only.  However, the address range covered   */
      /* by this entry is typically a subset of a previously          */
      /* encountered "LOAD" segment, so we need to exclude it.        */
      int j;

      start = (ptr_t)p->p_vaddr + info->dlpi_addr;
      end = start + p->p_memsz;
      for (j = n_load_segs; --j >= 0;) {
        if (ADDR_INSIDE(start, load_segs[j].start, load_segs[j].end)) {
          if (load_segs[j].start2 != NULL) {
            WARN("More than one GNU_RELRO segment per load one\n", 0);
          } else {
            GC_ASSERT(
                ADDR_GE(PTR_ALIGN_UP(load_segs[j].end, GC_page_size), end));
            /* Remove from the existing load segment. */
            load_segs[j].end2 = load_segs[j].end;
            load_segs[j].end = start;
            load_segs[j].start2 = end;
            /* Note that start2 may be greater than end2 because of   */
            /* p->p_memsz value multiple of page size.                */
          }
          break;
        }
        if (0 == j && 0 == GC_has_static_roots)
          WARN("Failed to find PT_GNU_RELRO segment"
               " inside PT_LOAD region\n",
               0);
        /* No warning reported in case of the callback is present   */
        /* because most likely the segment has been excluded.       */
      }
    }
  }
#        endif

  /* Signal that we were called.        */
  *(int *)ptr = 1;
  return 0;
}

/* Do we need to separately register the main static data segment? */
GC_INNER GC_bool
GC_register_main_static_data(void)
{
#        if defined(DL_ITERATE_PHDR_STRONG) && !defined(CPPCHECK)
  /* If dl_iterate_phdr is not a weak symbol then don't test against  */
  /* zero (otherwise a compiler might issue a warning).               */
  return FALSE;
#        else
  return 0 == COVERT_DATAFLOW(ADDR(dl_iterate_phdr));
#        endif
}

/* Return TRUE if we succeed, FALSE if dl_iterate_phdr wasn't there. */
STATIC GC_bool
GC_register_dynamic_libraries_dl_iterate_phdr(void)
{
  int did_something;

  GC_ASSERT(I_HOLD_LOCK());
  if (GC_register_main_static_data())
    return FALSE;

#        ifdef PT_GNU_RELRO
  {
    static GC_bool excluded_segs = FALSE;
    n_load_segs = 0;
    load_segs_overflow = FALSE;
    if (!EXPECT(excluded_segs, TRUE)) {
      GC_exclude_static_roots_inner((ptr_t)load_segs,
                                    (ptr_t)load_segs + sizeof(load_segs));
      excluded_segs = TRUE;
    }
  }
#        endif

  did_something = 0;
  dl_iterate_phdr(GC_register_dynlib_callback, &did_something);
  if (did_something) {
#        ifdef PT_GNU_RELRO
    int i;

    for (i = 0; i < n_load_segs; ++i) {
      if (ADDR_LT(load_segs[i].start, load_segs[i].end))
        GC_add_roots_inner(load_segs[i].start, load_segs[i].end, TRUE);
      if (ADDR_LT(load_segs[i].start2, load_segs[i].end2))
        GC_add_roots_inner(load_segs[i].start2, load_segs[i].end2, TRUE);
    }
#        endif
  } else {
    ptr_t datastart, dataend;
#        ifdef DATASTART_IS_FUNC
    static ptr_t datastart_cached = MAKE_CPTR(GC_WORD_MAX);

    /* Evaluate DATASTART only once.  */
    if (ADDR(datastart_cached) == GC_WORD_MAX) {
      datastart_cached = DATASTART;
    }
    datastart = datastart_cached;
#        else
    datastart = DATASTART;
#        endif
#        ifdef DATAEND_IS_FUNC
    {
      static ptr_t dataend_cached = 0;
      /* Evaluate DATAEND only once. */
      if (dataend_cached == 0) {
        dataend_cached = DATAEND;
      }
      dataend = dataend_cached;
    }
#        else
    dataend = DATAEND;
#        endif
    if (NULL == *(char *volatile *)&datastart || ADDR_LT(dataend, datastart))
      ABORT_ARG2("Wrong DATASTART/END pair", ": %p .. %p", (void *)datastart,
                 (void *)dataend);

    /* dl_iterate_phdr may forget the static data segment in  */
    /* statically linked executables.                         */
    GC_add_roots_inner(datastart, dataend, TRUE);
#        ifdef GC_HAVE_DATAREGION2
    /* Subtract one to check also for NULL without a compiler warning. */
    if (ADDR(DATASTART2) - 1U >= ADDR(DATAEND2)) {
      ABORT_ARG2("Wrong DATASTART/END2 pair", ": %p .. %p", (void *)DATASTART2,
                 (void *)DATAEND2);
    }
    GC_add_roots_inner(DATASTART2, DATAEND2, TRUE);
#        endif
  }
  return TRUE;
}

#        define HAVE_REGISTER_MAIN_STATIC_DATA

#      else /* !HAVE_DL_ITERATE_PHDR */

/* Dynamic loading code for Linux running ELF. Somewhat tested on
 * Linux/i686, untested but hopefully should work on Linux/Alpha.
 * This code was derived from the Solaris/ELF support. Thanks to
 * whatever kind soul wrote that.  - Patrick Bridges */

/* This doesn't necessarily work in all cases, e.g. with preloaded
 * dynamic libraries.                                           */

#        if defined(NETBSD) || defined(OPENBSD)
#          include <sys/exec_elf.h>
/* for compatibility with 1.4.x */
#          ifndef DT_DEBUG
#            define DT_DEBUG 21
#          endif
#          ifndef PT_LOAD
#            define PT_LOAD 1
#          endif
#          ifndef PF_W
#            define PF_W 2
#          endif
#        elif !defined(HOST_ANDROID)
#          include <elf.h>
#        endif

#        ifndef HOST_ANDROID
#          include <link.h>
#        endif

#      endif /* !HAVE_DL_ITERATE_PHDR */

EXTERN_C_BEGIN
#      ifdef __GNUC__
#        pragma weak _DYNAMIC
#      endif
extern ElfW(Dyn) _DYNAMIC[];
EXTERN_C_END

STATIC struct link_map *
GC_FirstDLOpenedLinkMap(void)
{
  static struct link_map *cachedResult = 0;

  if (0 == COVERT_DATAFLOW(ADDR(_DYNAMIC))) {
    /* _DYNAMIC symbol not resolved. */
    return NULL;
  }
  if (NULL == cachedResult) {
#      if defined(NETBSD) && defined(RTLD_DI_LINKMAP)
#        if defined(CPPCHECK)
#          define GC_RTLD_DI_LINKMAP 2
#        else
#          define GC_RTLD_DI_LINKMAP RTLD_DI_LINKMAP
#        endif
    struct link_map *lm = NULL;
    if (!dlinfo(RTLD_SELF, GC_RTLD_DI_LINKMAP, &lm) && lm != NULL) {
      /* Now lm points link_map object of libgc.  Since it    */
      /* might not be the first dynamically linked object,    */
      /* try to find it (object next to the main object).     */
      while (lm->l_prev != NULL) {
        lm = lm->l_prev;
      }
      cachedResult = lm->l_next;
    }
#      else
    ElfW(Dyn) * dp;
    int tag;

    for (dp = _DYNAMIC; (tag = dp->d_tag) != 0; dp++) {
      if (tag == DT_DEBUG) {
        const struct r_debug *rd = (struct r_debug *)dp->d_un.d_ptr;
        /* d_ptr could be null if libs are linked statically. */
        if (rd != NULL) {
          const struct link_map *lm = rd->r_map;
          if (lm != NULL)
            cachedResult = lm->l_next; /* might be NULL */
        }
        break;
      }
    }
#      endif /* !NETBSD || !RTLD_DI_LINKMAP */
  }
  return cachedResult;
}

GC_INNER void
GC_register_dynamic_libraries(void)
{
  struct link_map *lm;

  GC_ASSERT(I_HOLD_LOCK());
#      ifdef HAVE_DL_ITERATE_PHDR
  if (GC_register_dynamic_libraries_dl_iterate_phdr()) {
    return;
  }
#      endif
  for (lm = GC_FirstDLOpenedLinkMap(); lm != 0; lm = lm->l_next) {
    ElfW(Ehdr) * e;
    ElfW(Phdr) * p;
    unsigned long offset;
    char *start;
    int i;

    e = (ElfW(Ehdr) *)lm->l_addr;
#      ifdef HOST_ANDROID
    if (e == NULL)
      continue;
#      endif
    p = (ElfW(Phdr) *)((char *)e + e->e_phoff);
    offset = (unsigned long)lm->l_addr;
    for (i = 0; i < (int)e->e_phnum; i++, p++) {
      switch (p->p_type) {
      case PT_LOAD:
        if (!(p->p_flags & PF_W))
          break;
        start = (char *)p->p_vaddr + offset;
        GC_add_roots_inner(start, start + p->p_memsz, TRUE);
        break;
      default:
        break;
      }
    }
#      if defined(CPPCHECK) && defined(HOST_ANDROID) \
          && !defined(GC_DONT_DEFINE_LINK_MAP) && !(__ANDROID_API__ >= 21)
    GC_noop1_ptr(lm->l_name);
    GC_noop1((word)lm->l_ld);
    GC_noop1_ptr(lm->l_prev);
#      endif
  }
}

#    endif /* !USE_PROC_FOR_LIBRARIES */

#  endif /* DGUX || HURD || NACL || (ANY_BSD || LINUX) && __ELF__ */

#  if defined(IRIX5) || (defined(USE_PROC_FOR_LIBRARIES) && !defined(LINUX))

#    include <elf.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <signal.h> /* Only for the following test. */
#    include <sys/procfs.h>
#    include <sys/stat.h>
#    ifndef _sigargs
#      define IRIX6
#    endif

/* We use /proc to track down all parts of the address space that are   */
/* mapped by the process, and throw out regions we know we shouldn't    */
/* worry about.  This may also work under other SVR4 variants.          */
GC_INNER void
GC_register_dynamic_libraries(void)
{
  static int fd = -1;
  static prmap_t *addr_map = 0;
  /* Number of records currently in addr_map. */
  static int current_sz = 0;
  char buf[32];
  /* Required size of addr_map.       */
  int needed_sz = 0;
  int i;
  long flags;
  ptr_t start;
  ptr_t limit;
  word heap_start = ADDR(HEAP_START);
  word heap_end = heap_start;
#    ifdef SOLARISDL
#      define MA_PHYS 0
#    endif

  GC_ASSERT(I_HOLD_LOCK());
  if (fd < 0) {
    (void)snprintf(buf, sizeof(buf), "/proc/%ld", (long)getpid());
    buf[sizeof(buf) - 1] = '\0';
    fd = open(buf, O_RDONLY);
    if (fd < 0) {
      ABORT("/proc open failed");
    }
  }
  if (ioctl(fd, PIOCNMAP, &needed_sz) < 0) {
    ABORT_ARG2("/proc PIOCNMAP ioctl failed", ": fd= %d, errno= %d", fd,
               errno);
  }
  if (needed_sz >= current_sz) {
    GC_scratch_recycle_no_gww(addr_map, (size_t)current_sz * sizeof(prmap_t));
    /* Expansion, plus room for record 0.   */
    current_sz = needed_sz * 2 + 1;
    addr_map
        = (prmap_t *)GC_scratch_alloc((size_t)current_sz * sizeof(prmap_t));
    if (addr_map == NULL)
      ABORT("Insufficient memory for address map");
  }
  if (ioctl(fd, PIOCMAP, addr_map) < 0) {
    ABORT_ARG3("/proc PIOCMAP ioctl failed",
               ": errcode= %d, needed_sz= %d, addr_map= %p", errno, needed_sz,
               (void *)addr_map);
  }
  if (GC_n_heap_sects > 0) {
    heap_end = ADDR(GC_heap_sects[GC_n_heap_sects - 1].hs_start)
               + GC_heap_sects[GC_n_heap_sects - 1].hs_bytes;
    if (heap_end < GC_scratch_last_end_addr)
      heap_end = GC_scratch_last_end_addr;
  }
  for (i = 0; i < needed_sz; i++) {
    flags = addr_map[i].pr_mflags;
    if ((flags & (MA_BREAK | MA_STACK | MA_PHYS | MA_FETCHOP | MA_NOTCACHED))
        != 0)
      goto irrelevant;
    if ((flags & (MA_READ | MA_WRITE)) != (MA_READ | MA_WRITE))
      goto irrelevant;
    /* The latter test is empirically useless in very old Irix      */
    /* versions.  Other than the main data and stack segments,      */
    /* everything appears to be mapped readable, writable,          */
    /* executable, and shared(!!).  This makes no sense to me. - HB */
    start = (ptr_t)addr_map[i].pr_vaddr;
    if (GC_roots_present(start)
        || (ADDR(start) >= heap_start && ADDR(start) < heap_end))
      goto irrelevant;

    limit = start + addr_map[i].pr_size;
    /* The following seemed to be necessary for very old versions   */
    /* of Irix, but it has been reported to discard relevant        */
    /* segments under Irix 6.5.                                     */
#    ifndef IRIX6
    if (addr_map[i].pr_off == 0 && strncmp(start, ELFMAG, 4) == 0) {
      /* Discard text segments, i.e. 0-offset mappings against    */
      /* executable files which appear to have ELF headers.       */
      caddr_t arg;
      int obj;
#      define MAP_IRR_SZ 10
      /* Known irrelevant map entries.    */
      static ptr_t map_irr[MAP_IRR_SZ];
      static int n_irr = 0;
      struct stat buf;
      int j;

      for (j = 0; j < n_irr; j++) {
        if (map_irr[j] == start)
          goto irrelevant;
      }
      arg = (caddr_t)start;
      obj = ioctl(fd, PIOCOPENM, &arg);
      if (obj >= 0) {
        fstat(obj, &buf);
        close(obj);
        if ((buf.st_mode & 0111) != 0) {
          if (n_irr < MAP_IRR_SZ) {
            map_irr[n_irr++] = start;
          }
          goto irrelevant;
        }
      }
    }
#    endif /* !IRIX6 */
    GC_add_roots_inner(start, limit, TRUE);
  irrelevant:;
  }
  /* Don't keep cached descriptor, for now.  Some kernels don't like us */
  /* to keep a /proc file descriptor around during kill -9.             */
  /* Otherwise, it should also require FD_CLOEXEC and proper handling   */
  /* at fork (i.e. close because of the pid change).                    */
  if (close(fd) < 0)
    ABORT("Couldn't close /proc file");
  fd = -1;
}

#  endif /* USE_PROC_FOR_LIBRARIES || IRIX5 */

#  if defined(ALPHA) && defined(OSF1)
#    include <loader.h>

EXTERN_C_BEGIN
extern char *sys_errlist[];
extern int sys_nerr;
extern int errno;
EXTERN_C_END

GC_INNER void
GC_register_dynamic_libraries(void)
{
  ldr_module_t moduleid = LDR_NULL_MODULE;
  ldr_process_t mypid;

  GC_ASSERT(I_HOLD_LOCK());
  /* Obtain id of this process.       */
  mypid = ldr_my_process();

  /* For each module. */
  for (;;) {
    ldr_module_info_t moduleinfo;
    size_t modulereturnsize;
    ldr_region_t region;
    ldr_region_info_t regioninfo;
    size_t regionreturnsize;
    int status = ldr_next_module(mypid, &moduleid);
    /* Get the next (first) module */

    if (moduleid == LDR_NULL_MODULE) {
      /* No more modules.     */
      break;
    }

    /* Check status AFTER checking moduleid because       */
    /* of a bug in the non-shared ldr_next_module stub.   */
    if (status != 0) {
      ABORT_ARG3("ldr_next_module failed", ": status= %d, errcode= %d (%s)",
                 status, errno, errno < sys_nerr ? sys_errlist[errno] : "");
    }

    /* Get the module information.    */
    status = ldr_inq_module(mypid, moduleid, &moduleinfo, sizeof(moduleinfo),
                            &modulereturnsize);
    if (status != 0)
      ABORT("ldr_inq_module failed");

    /* Is module for the main program (i.e. nonshared portion)?   */
    if ((moduleinfo.lmi_flags & LDR_MAIN) != 0) {
      /* Skip the main module.        */
      continue;
    }

#    ifdef DL_VERBOSE
    GC_log_printf("---Module---\n");
    GC_log_printf("Module ID: %ld\n", moduleinfo.lmi_modid);
    GC_log_printf("Count of regions: %d\n", moduleinfo.lmi_nregion);
    GC_log_printf("Flags for module: %016lx\n", moduleinfo.lmi_flags);
    GC_log_printf("Module pathname: \"%s\"\n", moduleinfo.lmi_name);
#    endif

    /* For each region in this module. */
    for (region = 0; region < moduleinfo.lmi_nregion; region++) {
      /* Get the region information. */
      status = ldr_inq_region(mypid, moduleid, region, &regioninfo,
                              sizeof(regioninfo), &regionreturnsize);
      if (status != 0)
        ABORT("ldr_inq_region failed");

      /* Only process writable (data) regions.      */
      if ((regioninfo.lri_prot & LDR_W) == 0)
        continue;

#    ifdef DL_VERBOSE
      GC_log_printf("--- Region ---\n");
      GC_log_printf("Region number: %ld\n", regioninfo.lri_region_no);
      GC_log_printf("Protection flags: %016x\n", regioninfo.lri_prot);
      GC_log_printf("Virtual address: %p\n", regioninfo.lri_vaddr);
      GC_log_printf("Mapped address: %p\n", regioninfo.lri_mapaddr);
      GC_log_printf("Region size: %ld\n", regioninfo.lri_size);
      GC_log_printf("Region name: \"%s\"\n", regioninfo.lri_name);
#    endif

      /* Register region as a garbage collection root.      */
      GC_add_roots_inner((char *)regioninfo.lri_mapaddr,
                         (char *)regioninfo.lri_mapaddr + regioninfo.lri_size,
                         TRUE);
    }
  }
}
#  endif /* ALPHA && OSF1 */

#  if defined(HPUX)
#    include <dl.h>
#    include <errno.h>

EXTERN_C_BEGIN
extern char *sys_errlist[];
extern int sys_nerr;
EXTERN_C_END

GC_INNER void
GC_register_dynamic_libraries(void)
{
  /* Ordinal position in shared library search list.    */
  int index = 1;

  GC_ASSERT(I_HOLD_LOCK());
  /* For each dynamic library loaded. */
  for (;;) {
    /* Shared library info, see dl.h.        */
    struct shl_descriptor *shl_desc;
    /* Get info about next shared library.    */
    int status = shl_get(index, &shl_desc);

    /* Check if this is the end of the list or if some error occurred. */
    if (status != 0) {
#    if defined(HPUX) && defined(THREADS)
      /* I've seen errno values of 0.  The man page is not clear   */
      /* as to whether errno should get set on a -1 return.        */
      break;
#    else
      if (errno == EINVAL) {
        /* Moved past end of shared library list.  Finish.  */
        break;
      } else {
        ABORT_ARG3("shl_get failed", ": status= %d, errcode= %d (%s)", status,
                   errno, errno < sys_nerr ? sys_errlist[errno] : "");
      }
#    endif
    }

#    ifdef DL_VERBOSE
    GC_log_printf("---Shared library---\n");
    GC_log_printf("filename= \"%s\"\n", shl_desc->filename);
    GC_log_printf("index= %d\n", index);
    GC_log_printf("handle= %08x\n", (unsigned long)shl_desc->handle);
    GC_log_printf("text seg.start= %08x\n", shl_desc->tstart);
    GC_log_printf("text seg.end= %08x\n", shl_desc->tend);
    GC_log_printf("data seg.start= %08x\n", shl_desc->dstart);
    GC_log_printf("data seg.end= %08x\n", shl_desc->dend);
    GC_log_printf("ref.count= %lu\n", shl_desc->ref_count);
#    endif

    /* Register shared library's data segment as a garbage collection */
    /* root.                                                          */
    GC_add_roots_inner((char *)shl_desc->dstart, (char *)shl_desc->dend, TRUE);

    index++;
  }
}
#  endif /* HPUX */

#  ifdef AIX
#    include <alloca.h>
#    include <sys/errno.h>
#    include <sys/ldr.h>

GC_INNER void
GC_register_dynamic_libraries(void)
{
  int ldibuflen = 8192;

  GC_ASSERT(I_HOLD_LOCK());
  for (;;) {
    int len;
    struct ld_info *ldi;
#    if defined(CPPCHECK)
    char ldibuf[ldibuflen];
#    else
    char *ldibuf = alloca(ldibuflen);
#    endif

    len = loadquery(L_GETINFO, ldibuf, ldibuflen);
    if (len < 0) {
      if (errno != ENOMEM) {
        ABORT("loadquery failed");
      }
      ldibuflen *= 2;
      continue;
    }

    ldi = (struct ld_info *)ldibuf;
    while (ldi) {
      len = ldi->ldinfo_next;
      GC_add_roots_inner(ldi->ldinfo_dataorg,
                         MAKE_CPTR((unsigned long)ldi->ldinfo_dataorg)
                             + ldi->ldinfo_datasize,
                         TRUE);
      ldi = len ? (struct ld_info *)((char *)ldi + len) : 0;
    }
    break;
  }
}
#  endif /* AIX */

#  ifdef DARWIN

/* __private_extern__ hack required for pre-3.4 gcc versions.   */
#    ifndef __private_extern__
#      define __private_extern__ extern
#      include <mach-o/dyld.h>
#      undef __private_extern__
#    else
#      include <mach-o/dyld.h>
#    endif

#    if CPP_WORDSZ == 64
#      define GC_MACH_HEADER mach_header_64
#    else
#      define GC_MACH_HEADER mach_header
#    endif

#    ifdef MISSING_MACH_O_GETSECT_H
EXTERN_C_BEGIN
extern uint8_t *getsectiondata(const struct GC_MACH_HEADER *, const char *seg,
                               const char *sect, unsigned long *psz);
EXTERN_C_END
#    else
#      include <mach-o/getsect.h>
#    endif

/*#define DARWIN_DEBUG*/

/* Writable sections generally available on Darwin.     */
STATIC const struct dyld_sections_s {
  const char *seg;
  const char *sect;
} GC_dyld_sections[]
    = { { SEG_DATA, SECT_DATA },
        /* Used by FSF GCC, but not by OS X system tools, so far.   */
        { SEG_DATA, "__static_data" },
        { SEG_DATA, SECT_BSS },
        { SEG_DATA, SECT_COMMON },
        /* FSF GCC - zero-sized object sections for targets         */
        /* supporting section anchors.                              */
        { SEG_DATA, "__zobj_data" },
        { SEG_DATA, "__zobj_bss" } };

/* Additional writable sections:                                */
/* GCC on Darwin constructs aligned sections "on demand", where */
/* the alignment size is embedded in the section name.          */
/* Furthermore, there are distinctions between sections         */
/* containing private vs. public symbols.  It also constructs   */
/* sections specifically for zero-sized objects, when the       */
/* target supports section anchors.                             */
STATIC const char *const GC_dyld_bss_prefixes[]
    = { "__bss", "__pu_bss", "__zo_bss", "__zo_pu_bss" };

/* Currently, mach-o will allow up to the max of 2^15 alignment */
/* in an object file.                                           */
#    ifndef L2_MAX_OFILE_ALIGNMENT
#      define L2_MAX_OFILE_ALIGNMENT 15
#    endif

STATIC const char *
GC_dyld_name_for_hdr(const struct GC_MACH_HEADER *phdr)
{
  unsigned long i, count = _dyld_image_count();

  for (i = 0; i < count; i++) {
    if ((const struct GC_MACH_HEADER *)_dyld_get_image_header(i) == phdr)
      return _dyld_get_image_name(i);
  }
  /* TODO: probably ABORT in this case? */
  return NULL; /* not found */
}

/* getsectbynamefromheader is deprecated (first time in macOS 13.0),    */
/* getsectiondata (introduced in macOS 10.7) is used instead if exists. */
/* Define USE_GETSECTBYNAME to use the deprecated symbol, if needed.    */
#    if !defined(USE_GETSECTBYNAME) \
        && (MAC_OS_X_VERSION_MIN_REQUIRED < 1070 /*MAC_OS_X_VERSION_10_7*/)
#      define USE_GETSECTBYNAME
#    endif

static void
dyld_section_add_del(const struct GC_MACH_HEADER *phdr, intptr_t slide,
                     const char *dlpi_name, GC_has_static_roots_func callback,
                     const char *seg, const char *secnam, GC_bool is_add)
{
  unsigned long start, end, sec_size;
#    ifdef USE_GETSECTBYNAME
#      if CPP_WORDSZ == 64
  const struct section_64 *sec = getsectbynamefromheader_64(phdr, seg, secnam);
#      else
  const struct section *sec = getsectbynamefromheader(phdr, seg, secnam);
#      endif

  if (NULL == sec)
    return;
  sec_size = sec->size;
  start = slide + sec->addr;
#    else

  UNUSED_ARG(slide);
  sec_size = 0;
  start = (unsigned long)getsectiondata(phdr, seg, secnam, &sec_size);
  if (0 == start)
    return;
#    endif
  if (sec_size < sizeof(ptr_t))
    return;
  end = start + sec_size;
  if (is_add) {
    LOCK();
    /* The user callback is invoked holding the allocator lock.   */
    if (EXPECT(callback != 0, FALSE)
        && !callback(dlpi_name, (void *)start, (size_t)sec_size)) {
      UNLOCK();
      return; /* skip section */
    }
    GC_add_roots_inner((ptr_t)start, (ptr_t)end, FALSE);
    UNLOCK();
  } else {
    GC_remove_roots((void *)start, (void *)end);
  }
#    ifdef DARWIN_DEBUG
  GC_log_printf("%s section __DATA,%s at %p-%p (%lu bytes) from image %s\n",
                is_add ? "Added" : "Removed", secnam, (void *)start,
                (void *)end, sec_size, dlpi_name);
#    endif
}

static void
dyld_image_add_del(const struct GC_MACH_HEADER *phdr, intptr_t slide,
                   GC_has_static_roots_func callback, GC_bool is_add)
{
  unsigned i, j;
  const char *dlpi_name;

  GC_ASSERT(I_DONT_HOLD_LOCK());
#    ifndef DARWIN_DEBUG
  if (0 == callback) {
    dlpi_name = NULL;
  } else
#    endif
  /* else */ {
    dlpi_name = GC_dyld_name_for_hdr(phdr);
  }
  for (i = 0; i < sizeof(GC_dyld_sections) / sizeof(GC_dyld_sections[0]);
       i++) {
    dyld_section_add_del(phdr, slide, dlpi_name, callback,
                         GC_dyld_sections[i].seg, GC_dyld_sections[i].sect,
                         is_add);
  }

  /* Sections constructed on demand.    */
  for (j = 0; j < sizeof(GC_dyld_bss_prefixes) / sizeof(char *); j++) {
    /* Our manufactured aligned BSS sections.   */
    for (i = 0; i <= L2_MAX_OFILE_ALIGNMENT; i++) {
      char secnam[16];

      (void)snprintf(secnam, sizeof(secnam), "%s%u", GC_dyld_bss_prefixes[j],
                     i);
      secnam[sizeof(secnam) - 1] = '\0';
      dyld_section_add_del(phdr, slide, dlpi_name, 0 /* callback */, SEG_DATA,
                           secnam, is_add);
    }
  }

#    if defined(DARWIN_DEBUG) && !defined(NO_DEBUGGING)
  READER_LOCK();
  GC_print_static_roots();
  READER_UNLOCK();
#    endif
}

STATIC void
GC_dyld_image_add(const struct GC_MACH_HEADER *phdr, intptr_t slide)
{
  if (!GC_no_dls)
    dyld_image_add_del(phdr, slide, GC_has_static_roots, TRUE);
}

STATIC void
GC_dyld_image_remove(const struct GC_MACH_HEADER *phdr, intptr_t slide)
{
  dyld_image_add_del(phdr, slide, 0 /* callback */, FALSE);
}

GC_INNER void
GC_register_dynamic_libraries(void)
{
  /* Currently does nothing. The callbacks are setup by GC_init_dyld()
  The dyld library takes it from there. */
}

/* The _dyld_* functions have an internal lock, so none of them can be  */
/* called while the world is stopped without the risk of a deadlock.    */
/* Because of this we MUST setup callbacks BEFORE we ever stop the      */
/* world.  This should be called BEFORE any thread is created and       */
/* WITHOUT the allocator lock held.                                     */

/* _dyld_bind_fully_image_containing_address is deprecated, so use      */
/* dlopen(0,RTLD_NOW) instead; define USE_DYLD_TO_BIND to override this */
/* if needed.                                                           */

GC_INNER void
GC_init_dyld(void)
{
  static GC_bool initialized = FALSE;

  GC_ASSERT(I_DONT_HOLD_LOCK());
  if (initialized)
    return;

#    ifdef DARWIN_DEBUG
  GC_log_printf("Registering dyld callbacks...\n");
#    endif

  /* Apple's Documentation:
     When you call _dyld_register_func_for_add_image, the dynamic linker
     runtime calls the specified callback (func) once for each of the images
     that is currently loaded into the program. When a new image is added to
     the program, your callback is called again with the mach_header for the
     new image, and the virtual memory slide amount of the new image.

     This WILL properly register already linked libraries and libraries
     linked in the future.
  */

  /* Structure mach_header_64 has the same fields as mach_header except */
  /* for the reserved one at the end, so these casts are OK.            */
  _dyld_register_func_for_add_image(
      (void (*)(const struct mach_header *, intptr_t))GC_dyld_image_add);
  _dyld_register_func_for_remove_image(
      (void (*)(const struct mach_header *, intptr_t))GC_dyld_image_remove);

  /* Set this early to avoid reentrancy issues. */
  initialized = TRUE;

#    ifndef NO_DYLD_BIND_FULLY_IMAGE
  if (GC_no_dls)
    return; /* skip main data segment registration */

  /* When the environment variable is set, the dynamic linker binds   */
  /* all undefined symbols the application needs at launch time.      */
  /* This includes function symbols that are normally bound lazily at */
  /* the time of their first invocation.                              */
  if (GETENV("DYLD_BIND_AT_LAUNCH") != NULL)
    return;

    /* The environment variable is unset, so we should bind manually.   */
#      ifdef DARWIN_DEBUG
  GC_log_printf("Forcing full bind of GC code...\n");
#      endif
#      ifndef USE_DYLD_TO_BIND
  {
    void *dl_handle = dlopen(NULL, RTLD_NOW);

    if (!dl_handle)
      ABORT("dlopen failed (to bind fully image)");
      /* Note that the handle is never closed.        */
#        if defined(CPPCHECK) || defined(LINT2)
    GC_noop1_ptr(dl_handle);
#        endif
  }
#      else
  /* Note: '_dyld_bind_fully_image_containing_address' is deprecated. */
  if (!_dyld_bind_fully_image_containing_address((unsigned long *)GC_malloc))
    ABORT("_dyld_bind_fully_image_containing_address failed");
#      endif
#    endif
}

#    define HAVE_REGISTER_MAIN_STATIC_DATA
GC_INNER GC_bool
GC_register_main_static_data(void)
{
  /* Already done through dyld callbacks */
  return FALSE;
}

#  endif /* DARWIN */

#  if defined(HAIKU)
#    include <kernel/image.h>

GC_INNER void
GC_register_dynamic_libraries(void)
{
  image_info info;
  int32 cookie = 0;

  GC_ASSERT(I_HOLD_LOCK());
  while (get_next_image_info(0, &cookie, &info) == B_OK) {
    ptr_t data = (ptr_t)info.data;
    GC_add_roots_inner(data, data + info.data_size, TRUE);
  }
}
#  endif /* HAIKU */

#endif /* DYNAMIC_LOADING */

#ifdef GC_MUST_RESTORE_REDEFINED_DLOPEN
#  define dlopen GC_dlopen
#endif

#if !defined(HAVE_REGISTER_MAIN_STATIC_DATA) && defined(DYNAMIC_LOADING)
/* Do we need to separately register the main static data segment? */
GC_INNER GC_bool
GC_register_main_static_data(void)
{
  return TRUE;
}
#endif /* HAVE_REGISTER_MAIN_STATIC_DATA */

/* Register a routine to filter dynamic library registration.  */
GC_API void GC_CALL
GC_register_has_static_roots_callback(GC_has_static_roots_func callback)
{
  GC_has_static_roots = callback;
}
