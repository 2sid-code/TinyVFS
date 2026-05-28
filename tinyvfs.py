import click
import ctypes
import os
import sys

# =============================================================================
# 1. Resolve C Shared Library
# =============================================================================
def getLibPath():
    if getattr(sys, 'frozen', False) and hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, "libtinyvfs.so")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "libtinyvfs.so")

libPath = getLibPath()
if not os.path.exists(libPath):
    click.secho(f"Error: Could not find compiled C library at {libPath}", fg="red", err=True)
    sys.exit(1)

vfsLib = ctypes.CDLL(libPath)

# =============================================================================
# 2. C Types & Struct Mappings
# =============================================================================
class Address(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("clusterAddr", ctypes.c_uint16),
        ("pageAddr", ctypes.c_uint8)
    ]

class TableEntry(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("name", ctypes.c_char * 27),
        ("address", Address),
        ("pagesOccupied", ctypes.c_uint16)
    ]

# Function Signatures
vfsLib.createRawDisk.argtypes = [ctypes.c_char_p, ctypes.c_int]
vfsLib.createFile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint16, ctypes.c_int]
vfsLib.readFile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_size_t)]
vfsLib.getFileSize.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
vfsLib.deleteFile.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
vfsLib.readTable.argtypes = [ctypes.c_char_p]

# =============================================================================
# 3. CLI Definition
# =============================================================================
@click.group()
def cli():
    """TinyVFS Manager - High-Performance C Engine via Python"""
    pass

@cli.command()
@click.argument('disk')
@click.option('--size', default=1048576, type=int, help='Size of the disk in bytes')
def create(disk, size):
    """Format a new virtual disk"""
    clusters = max(1, size // 65536)
    click.echo(f"Formatting '{disk}' ({size} bytes / {clusters} clusters)...")
    
    if vfsLib.createRawDisk(disk.encode(), clusters) == 0:
        click.secho("Success! Virtual disk created.", fg="green")
    else:
        click.secho("Failed to format disk.", fg="red")

@cli.command('import')
@click.argument('disk', type=click.Path(exists=True))
@click.argument('host_path', type=click.Path(exists=True, readable=True))
@click.argument('vfs_path')
def import_file(disk, host_path, vfs_path):
    """Import a file from the host OS to the VFS"""
    with open(host_path, "rb") as f:
        fileData = f.read()
        
    size = len(fileData)
    clusters = os.path.getsize(disk) // 65536
    
    dataC = (ctypes.c_uint8 * size).from_buffer_copy(fileData)
    
    result = vfsLib.createFile(disk.encode(), vfs_path.encode(), dataC, size, clusters)
    if result == 0:
        click.secho(f"Imported '{host_path}' -> '{vfs_path}' ({size} bytes)", fg="green")
    else:
        click.secho(f"Import failed. Error code: {result}", fg="red")

@cli.command('export')
@click.argument('disk', type=click.Path(exists=True))
@click.argument('vfs_path')
@click.argument('host_path')
def export_file(disk, vfs_path, host_path):
    """Export a file from the VFS to the host OS"""
    diskC = disk.encode()
    vfsPathC = vfs_path.encode()
    
    # Query the C engine for the allocation size
    size = vfsLib.getFileSize(diskC, vfsPathC)
    if size < 0:
        click.secho("Error: File not found in VFS.", fg="red", err=True)
        return
        
    buffer = (ctypes.c_uint8 * size)()
    bytes_read = ctypes.c_size_t(0)
    
    if vfsLib.readFile(diskC, vfsPathC, buffer, ctypes.byref(bytes_read)) == 0:
        with open(host_path, "wb") as f:
            f.write(bytes(buffer)[:bytes_read.value])
        click.secho(f"Exported '{vfs_path}' -> '{host_path}' ({bytes_read.value} bytes)", fg="green")
    else:
        click.secho("Export failed during read.", fg="red")

@cli.command('rm')
@click.argument('disk', type=click.Path(exists=True))
@click.argument('vfs_path')
def rm_file(disk, vfs_path):
    """Delete a file from the VFS"""
    if vfsLib.deleteFile(disk.encode(), vfs_path.encode()) == 0:
        click.secho(f"Deleted '{vfs_path}'", fg="green")
    else:
        click.secho(f"Failed to delete '{vfs_path}'. Not found?", fg="red")

@cli.command('ls')
@click.argument('disk', type=click.Path(exists=True))
def list_files(disk):
    """List all files mapped in the VFS root directory"""
    if vfsLib.readTable(disk.encode()) != 0:
        click.secho("Error reading VFS partition table.", fg="red", err=True)
        return
        
    # Map the exported global 'loadedTable' array directly into Python memory
    MAX_ENTRIES = 65536 // ctypes.sizeof(TableEntry)
    table = (TableEntry * MAX_ENTRIES).in_dll(vfsLib, "loadedTable")
    
    click.echo(f"{'Filename':<28} | {'Size (Pages)':<12} | Location (C/P)")
    click.echo("-" * 65)
    
    count = 0
    for entry in table:
        if entry.name: # If it's not b"", the slot is occupied
            name = entry.name.decode('utf-8', errors='ignore')
            pages = entry.pagesOccupied
            c_addr = entry.address.clusterAddr
            p_addr = entry.address.pageAddr
            click.echo(f"{name:<28} | {pages:<12} | {c_addr}/{p_addr}")
            count += 1
            
    if count == 0:
        click.echo("VFS is empty.")

if __name__ == '__main__':
    cli()