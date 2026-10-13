#!/usr/bin/env python3
import sys
from pathlib import Path

if len(sys.argv) != 5:
    raise SystemExit("usage: smb_vfs_patch.py <wine-file.c> <virtual_ios.c> <out-file.c> <out-virtual.c>")

file_src, virt_src, file_out, virt_out = map(Path, sys.argv[1:])
f = file_src.read_text()
v = virt_src.read_text()

decl = r'''
#ifdef WINE_IOS
/* Madeira direct-SMB sparse VFS. Implemented by the app target. */
extern int madeira_direct_smb_prefetch_fd( int fd, unsigned long long offset, size_t length );
extern int madeira_direct_smb_materialize_fd( int fd );
#endif
'''

anchor_decl_f = 'WINE_DECLARE_DEBUG_CHANNEL(winediag);\n'
if anchor_decl_f not in f:
    raise SystemExit("file.c: debug-channel anchor not found")
f = f.replace(anchor_decl_f, anchor_decl_f + decl, 1)

read_anchor = '''    status = server_get_unix_fd( handle, FILE_READ_DATA, &unix_handle, &needs_close, &type, &options );
    if (status && status != STATUS_BAD_DEVICE_TYPE) return status;

    if (!virtual_check_buffer_for_write( buffer, length )) return STATUS_ACCESS_VIOLATION;
'''
read_repl = '''    status = server_get_unix_fd( handle, FILE_READ_DATA, &unix_handle, &needs_close, &type, &options );
    if (status && status != STATUS_BAD_DEVICE_TYPE) return status;

#ifdef WINE_IOS
    if (!status && length)
    {
        off_t madeira_smb_off = offset ? (off_t)offset->QuadPart : lseek( unix_handle, 0, SEEK_CUR );
        if (madeira_smb_off >= 0)
        {
            int madeira_smb_rc = madeira_direct_smb_prefetch_fd( unix_handle,
                                                                  (unsigned long long)madeira_smb_off,
                                                                  (size_t)length );
            if (madeira_smb_rc < 0)
            {
                if (needs_close) close( unix_handle );
                return errno_to_status( -madeira_smb_rc );
            }
        }
    }
#endif

    if (!virtual_check_buffer_for_write( buffer, length )) return STATUS_ACCESS_VIOLATION;
'''
if read_anchor not in f:
    raise SystemExit("file.c: NtReadFile anchor not found")
f = f.replace(read_anchor, read_repl, 1)

scatter_anchor = '''    status = server_get_unix_fd( file, FILE_READ_DATA, &unix_handle, &needs_close, &type, &options );
    if (status) return status;

    if ((type != FD_TYPE_FILE) ||
'''
scatter_repl = '''    status = server_get_unix_fd( file, FILE_READ_DATA, &unix_handle, &needs_close, &type, &options );
    if (status) return status;

#ifdef WINE_IOS
    {
        int madeira_smb_rc = madeira_direct_smb_materialize_fd( unix_handle );
        if (madeira_smb_rc < 0)
        {
            if (needs_close) close( unix_handle );
            return errno_to_status( -madeira_smb_rc );
        }
    }
#endif

    if ((type != FD_TYPE_FILE) ||
'''
if scatter_anchor not in f:
    raise SystemExit("file.c: NtReadFileScatter anchor not found")
f = f.replace(scatter_anchor, scatter_repl, 1)

anchor_decl_v = '''WINE_DEFAULT_DEBUG_CHANNEL(virtual);
WINE_DECLARE_DEBUG_CHANNEL(module);
WINE_DECLARE_DEBUG_CHANNEL(virtual_ranges);
'''
if anchor_decl_v not in v:
    raise SystemExit("virtual_ios.c: debug-channel anchor not found")
v = v.replace(anchor_decl_v, anchor_decl_v + decl, 1)

mapping_anchor = '''    if ((status = server_get_unix_fd( mapping, 0, &unix_fd, &needs_close, NULL, NULL )))
        return status;

'''
mapping_repl = '''    if ((status = server_get_unix_fd( mapping, 0, &unix_fd, &needs_close, NULL, NULL )))
        return status;
#ifdef WINE_IOS
    {
        int madeira_smb_rc = madeira_direct_smb_materialize_fd( unix_fd );
        if (madeira_smb_rc < 0)
        {
            if (needs_close) close( unix_fd );
            return errno_to_status( -madeira_smb_rc );
        }
    }
#endif

'''
mapping_hits = v.count(mapping_anchor)
if mapping_hits < 1:
    raise SystemExit("virtual_ios.c: mapping fd anchor not found")
v = v.replace(mapping_anchor, mapping_repl)

handle_anchor = '''    if ((res = server_get_unix_fd( handle, 0, &unix_handle, &needs_close, NULL, NULL )))
    {
        dprintf(2, "[map-sec] server_get_unix_fd handle=%p -> 0x%x\\n", handle, res);
        return res;
    }

'''
handle_repl = '''    if ((res = server_get_unix_fd( handle, 0, &unix_handle, &needs_close, NULL, NULL )))
    {
        dprintf(2, "[map-sec] server_get_unix_fd handle=%p -> 0x%x\\n", handle, res);
        return res;
    }
#ifdef WINE_IOS
    {
        int madeira_smb_rc = madeira_direct_smb_materialize_fd( unix_handle );
        if (madeira_smb_rc < 0)
        {
            if (needs_close) close( unix_handle );
            return errno_to_status( -madeira_smb_rc );
        }
    }
#endif

'''
handle_hits = v.count(handle_anchor)
if handle_hits < 1:
    raise SystemExit("virtual_ios.c: map-sec fd anchor not found")
v = v.replace(handle_anchor, handle_repl)

file_out.write_text(f)
virt_out.write_text(v)
print("patched file.c: NtReadFile + NtReadFileScatter")
print(f"patched virtual_ios.c: mapping anchors={mapping_hits}, section anchors={handle_hits}")
