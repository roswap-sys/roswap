// SPDX-License-Identifier: GPL-2.0
use crate::sys;
use std::ffi::c_void;
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

pub const PAGE_SIZE: usize = 4096;

pub struct Chunk {
    pub gid: u32,
    pub epoch: u32,
    pub file_path: PathBuf,
    mr: NonNull<sys::ibv_mr>,
    addr: NonNull<c_void>,
    len: usize,
}

unsafe impl Send for Chunk {}
unsafe impl Sync for Chunk {}

impl Chunk {
    pub fn create(
        pd: NonNull<sys::ibv_pd>,
        dir: &Path,
        gid: u32,
        epoch: u32,
        len: usize,
    ) -> io::Result<Self> {
        if len == 0 || len % (1 << 20) != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "chunk size must be a positive multiple of 1 MiB",
            ));
        }

        fs::create_dir_all(dir)?;
        let file_path = dir.join(format!("chunk-{gid}-{epoch}"));
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(&file_path)?;
        file.set_len(len as u64)?;

        let raw_addr = unsafe {
            sys::mmap(
                std::ptr::null_mut(),
                len,
                sys::PROT_READ | sys::PROT_WRITE,
                sys::MAP_SHARED | sys::MAP_POPULATE,
                file.as_raw_fd(),
                0,
            )
        };
        if raw_addr == sys::MAP_FAILED || raw_addr.is_null() {
            return Err(io::Error::last_os_error());
        }
        let addr = unsafe { NonNull::new_unchecked(raw_addr) };

        unsafe {
            let _ = sys::madvise(addr.as_ptr(), len, sys::MADV_HUGEPAGE);
        }

        let access =
            sys::IBV_ACCESS_LOCAL_WRITE | sys::IBV_ACCESS_REMOTE_READ | sys::IBV_ACCESS_REMOTE_WRITE;
        let raw_mr = unsafe { sys::ibv_reg_mr(pd.as_ptr(), addr.as_ptr(), len, access) };
        let mr = match NonNull::new(raw_mr) {
            Some(mr) => mr,
            None => {
                let err = io::Error::last_os_error();
                unsafe {
                    sys::munmap(addr.as_ptr(), len);
                }
                return Err(err);
            }
        };

        Ok(Self {
            gid,
            epoch,
            file_path,
            mr,
            addr,
            len,
        })
    }

    pub fn addr(&self) -> u64 {
        self.addr.as_ptr() as u64
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn lkey(&self) -> u32 {
        unsafe { self.mr.as_ref().lkey }
    }

    pub fn rkey(&self) -> u32 {
        unsafe { self.mr.as_ref().rkey }
    }

    pub fn as_slice(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.addr.as_ptr() as *const u8, self.len) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.addr.as_ptr() as *mut u8, self.len) }
    }

    pub fn release_and_unlink(self) -> io::Result<()> {
        let path = self.file_path.clone();
        drop(self);
        fs::remove_file(path)
    }
}

impl Drop for Chunk {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::ibv_dereg_mr(self.mr.as_ptr());
            let _ = sys::munmap(self.addr.as_ptr(), self.len);
        }
    }
}

#[repr(C, align(4096))]
pub struct HeartbeatLayout {
    pub tick_counter: AtomicU64,
    pub boot_id_lo: u64,
    pub node_epoch: u32,
    pub status_flags: u32,
    _pad: [u8; PAGE_SIZE - 24],
}

pub struct HeartbeatPage {
    mr: NonNull<sys::ibv_mr>,
    ptr: NonNull<HeartbeatLayout>,
}

unsafe impl Send for HeartbeatPage {}
unsafe impl Sync for HeartbeatPage {}

impl HeartbeatPage {
    pub fn new(pd: NonNull<sys::ibv_pd>, node_epoch: u32, boot_id_lo: u64) -> io::Result<Self> {
        let mut raw_ptr: *mut c_void = std::ptr::null_mut();
        let rc = unsafe { sys::posix_memalign(&mut raw_ptr, PAGE_SIZE, PAGE_SIZE) };
        if rc != 0 || raw_ptr.is_null() {
            return Err(io::Error::from_raw_os_error(rc));
        }

        unsafe {
            std::ptr::write_bytes(raw_ptr as *mut u8, 0, PAGE_SIZE);
        }
        let ptr = unsafe { NonNull::new_unchecked(raw_ptr as *mut HeartbeatLayout) };
        unsafe {
            (*ptr.as_ptr()).tick_counter = AtomicU64::new(1);
            (*ptr.as_ptr()).boot_id_lo = boot_id_lo;
            (*ptr.as_ptr()).node_epoch = node_epoch;
            (*ptr.as_ptr()).status_flags = 1;
        }

        let access = sys::IBV_ACCESS_LOCAL_WRITE | sys::IBV_ACCESS_REMOTE_READ;
        let raw_mr = unsafe { sys::ibv_reg_mr(pd.as_ptr(), raw_ptr, PAGE_SIZE, access) };
        let mr = match NonNull::new(raw_mr) {
            Some(m) => m,
            None => {
                let err = io::Error::last_os_error();
                unsafe {
                    sys::free(raw_ptr);
                }
                return Err(err);
            }
        };

        Ok(Self { mr, ptr })
    }

    pub fn tick(&self) -> u64 {
        unsafe {
            self.ptr
                .as_ref()
                .tick_counter
                .fetch_add(1, Ordering::Release)
                + 1
        }
    }

    pub fn read_tick(&self) -> u64 {
        unsafe { self.ptr.as_ref().tick_counter.load(Ordering::Acquire) }
    }

    pub fn addr(&self) -> u64 {
        self.ptr.as_ptr() as u64
    }

    pub fn lkey(&self) -> u32 {
        unsafe { self.mr.as_ref().lkey }
    }

    pub fn rkey(&self) -> u32 {
        unsafe { self.mr.as_ref().rkey }
    }
}

impl Drop for HeartbeatPage {
    fn drop(&mut self) {
        unsafe {
            let _ = sys::ibv_dereg_mr(self.mr.as_ptr());
            sys::free(self.ptr.as_ptr() as *mut c_void);
        }
    }
}
