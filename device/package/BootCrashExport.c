// SPDX-License-Identifier: MIT
// Best-effort CrashReporter snapshot for the UFS log. Runs on the boot-log
// thread every few rounds and copies the newest .ips from the Data volume
// into the tail of the export region, where UEFI's UFS Log reads SHCRASH01.
//
// Compiled in the BSD source context because directory walking needs
// vnode/uio; BootLog.c stays in the osfmk context and only owns the storage
// window.
#include "BootCrashExport.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <IOKit/IOLib.h>

#define BOOT_CRASH_DIRECTORY "/private/var/mobile/Library/Logs/CrashReporter"
#define BOOT_CRASH_ENTRIES_SIZE (64u * 1024u)
#define BOOT_CRASH_MAX_FILE_BYTES (4u * 1024u * 1024u)

// Every report by name and size, independent of the clock: the device clock restarts at 1969-12-31 16:00 on every boot, so the newest
// report by time is often one of an earlier boot and this boot's crashes never reached the console (204929: backboardd restarted twice
// while the excerpt still showed the previous boot's 16:30:34 report). The first pass takes what is on the Data volume as known (names
// only, except the wireless daemon's); a report that appears later is this boot's: it is named and, for the processes below, excerpted.
#define BOOT_CRASH_KNOWN 256u
static uint32_t known_hash[BOOT_CRASH_KNOWN];
static unsigned known_count, collect_passes;
static uint32_t report_hash(const char *name, uint64_t size) {
  uint32_t h = 2166136261u;
  for (const char *c = name; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  for (unsigned i = 0; i < 8; ++i) h = (h ^ (uint8_t)(size >> (8 * i))) * 16777619u;
  return h;
}
static int report_known(uint32_t h) {
  for (unsigned i = 0; i < known_count; ++i) if (known_hash[i] == h) return 1;
  return 0;
}
static void report_remember(uint32_t h) {if (known_count < BOOT_CRASH_KNOWN) known_hash[known_count++] = h;}
static int report_interesting(const char *name) {
  return !strncmp(name, "Preferences", 11) || !strncmp(name, "SpringBoard", 11) || !strncmp(name, "backboardd", 10) ||
         !strncmp(name, "JetsamEvent", 11) || !strncmp(name, "stacks", 6) || !strncmp(name, "wifid", 5);
}
static void report_emit(const char *name, const uint8_t *payload, uint32_t size) {
  const uint32_t limit = size < 65536 ? size : 65536;
  IOLog("[UiCrash BEGIN] %s bytes=%u excerpt=%u\n", name, size, limit);
  for (uint32_t pos = 0; pos < limit; pos += 192) {char part[193]; uint32_t n = limit - pos < 192 ? limit - pos : 192;
    memcpy(part, payload + pos, n); part[n] = 0; IOLog("%s", part);}
  IOLog("\n[UiCrash END]\n");
}
// One report into payload; its length read, 0 on failure.
static uint32_t report_read(vnode_t dir, const char *name, uint64_t size, uint8_t *payload, vfs_context_t ctx) {
  if (size > BOOT_CRASH_MAX_FILE_BYTES) size = BOOT_CRASH_MAX_FILE_BYTES;
  vnode_t child = NULLVP; uint32_t got = 0;
  int error = vnode_lookupat(name, VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT, &child, ctx, dir);
  if (!error && vnode_vtype(child) == VREG && !VNOP_OPEN(child, FREAD, ctx)) {
    memset(payload, 0, BOOT_CRASH_MAX_FILE_BYTES);
    int resid = 0;
    error = vn_rdwr(UIO_READ, child, (caddr_t)payload, (int)size, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    VNOP_CLOSE(child, FREAD, ctx);
    if (!error && resid >= 0 && (uint64_t)resid < size) got = (uint32_t)(size - (uint64_t)resid);
  }
  if (child != NULLVP) vnode_put(child);
  return got;
}

void boot_log_crash_collect(void) {
  vfs_context_t ctx = vfs_context_current();
  vnode_t dir = NULLVP;
  int error = vnode_lookup(BOOT_CRASH_DIRECTORY, VNODE_LOOKUP_NOFOLLOW, &dir, ctx);
  if (error) return;
  if (vnode_vtype(dir) != VDIR) {vnode_put(dir); return;}
  error = VNOP_OPEN(dir, FREAD, ctx);
  if (error) {vnode_put(dir); return;}

  uint8_t *entries = (uint8_t *)IOMallocAligned(BOOT_CRASH_ENTRIES_SIZE, 4096);
  uint8_t *payload = (uint8_t *)IOMallocAligned(BOOT_CRASH_MAX_FILE_BYTES, 4096);
  char selected[256]; selected[0] = 0;
  uint64_t selected_time = 0, selected_size = 0;
  int found = 0;
  static char pending[4][256]; static uint64_t pending_size[4]; unsigned pending_count = 0;
  const int first_pass = collect_passes == 0;
  if (entries && payload) {
    off_t offset = 0;
    for (int eof = 0; !error && !eof;) {
      uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
      if (!uio) {error = ENOMEM; break;}
      uio_addiov(uio, CAST_USER_ADDR_T(entries), BOOT_CRASH_ENTRIES_SIZE);
      int count = 0;
      error = vnode_readdir64(dir, uio, VNODE_READDIR_EXTENDED, &eof, &count, ctx);
      const uint32_t filled = BOOT_CRASH_ENTRIES_SIZE - (uint32_t)uio_resid(uio);
      offset = uio_offset(uio);
      uio_free(uio);
      if (error || !filled) break;
      for (uint32_t at = 0; at + offsetof(struct direntry, d_name) < filled;) {
        struct direntry *entry = (struct direntry *)(entries + at);
        if (!entry->d_reclen || at + entry->d_reclen > filled) {error = EINVAL; break;}
        at += entry->d_reclen;
        const char *name = entry->d_name;
        if (!entry->d_ino || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        const size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".ips")) continue;
        vnode_t child = NULLVP;
        int child_error = vnode_lookupat(name, VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT, &child, ctx, dir);
        if (child_error) continue;
        if (vnode_vtype(child) == VREG) {
          struct vnode_attr va;
          VATTR_INIT(&va);
          VATTR_WANTED(&va, va_data_size);
          VATTR_WANTED(&va, va_modify_time);
          child_error = vnode_getattr(child, &va, ctx);
          if (!child_error && va.va_data_size) {
            const uint32_t h = report_hash(name, (uint64_t)va.va_data_size);
            const int wanted = first_pass ? !strncmp(name, "wifid", 5) : report_interesting(name);
            if (!report_known(h) && (!wanted || pending_count < 4)) {   // a wanted one beyond four waits for the next pass
              report_remember(h);
              IOLog("[CrashList] %s %s %llu\n", first_pass ? "old" : "new", name, (unsigned long long)va.va_data_size);
              if (wanted) {
                strlcpy(pending[pending_count], name, sizeof pending[0]); pending_size[pending_count] = (uint64_t)va.va_data_size; ++pending_count;
              }
            }
            const uint64_t t = (uint64_t)va.va_modify_time.tv_sec * 1000000000ull + (uint64_t)va.va_modify_time.tv_nsec;
            if (!found || t > selected_time) {
              strlcpy(selected, name, sizeof selected);
              selected_time = t; selected_size = (uint64_t)va.va_data_size; found = 1;
            }
          }
        }
        vnode_put(child);
      }
    }
  }

  const char *stored_name = boot_log_crash_current_name();
  uint32_t stored_bytes = boot_log_crash_current_bytes();
  if (found && (strcmp(selected, stored_name) || selected_size != stored_bytes)) {
    if (selected_size > BOOT_CRASH_MAX_FILE_BYTES) selected_size = BOOT_CRASH_MAX_FILE_BYTES;
    vnode_t child = NULLVP;
    error = vnode_lookupat(selected, VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT, &child, ctx, dir);
    if (!error && vnode_vtype(child) == VREG) {
      error = VNOP_OPEN(child, FREAD, ctx);
      if (!error) {
        memset(payload, 0, BOOT_CRASH_MAX_FILE_BYTES);
        int resid = 0;
        error = vn_rdwr(UIO_READ, child, (caddr_t)payload, (int)selected_size, 0, UIO_SYSSPACE, IO_NOAUTH,
                        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        VNOP_CLOSE(child, FREAD, ctx);
        if (!error && resid >= 0 && (uint64_t)resid < selected_size) {
          boot_log_crash_store(selected, payload, (uint32_t)(selected_size - (uint64_t)resid));
        }
      }
    }
    if (child != NULLVP) vnode_put(child);
  }
  // this boot's reports (and an old one of the wireless daemon), each once
  if (payload) for (unsigned i = 0; i < pending_count; ++i) {
    const uint32_t got = report_read(dir, pending[i], pending_size[i], payload, ctx);
    if (got) report_emit(pending[i], payload, got);
  }
  ++collect_passes;

  if (entries) IOFreeAligned(entries, BOOT_CRASH_ENTRIES_SIZE);
  if (payload) IOFreeAligned(payload, BOOT_CRASH_MAX_FILE_BYTES);
  VNOP_CLOSE(dir, FREAD, ctx);
  vnode_put(dir);
}
