# 0 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
module;
# 1 "/usr/include/fcntl.h" 1 3
# 25 "/usr/include/fcntl.h" 3
# 1 "/usr/include/features.h" 1 3
# 431 "/usr/include/features.h" 3
# 1 "/usr/include/features-time64.h" 1 3
# 20 "/usr/include/features-time64.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 21 "/usr/include/features-time64.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/timesize.h" 1 3
# 22 "/usr/include/features-time64.h" 2 3
# 432 "/usr/include/features.h" 2 3
# 539 "/usr/include/features.h" 3
# 1 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 1 3
# 730 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 731 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/long-double.h" 1 3
# 732 "/usr/include/aarch64-linux-gnu/sys/cdefs.h" 2 3
# 540 "/usr/include/features.h" 2 3
# 563 "/usr/include/features.h" 3
# 1 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 1 3




# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 6 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 2 3


# 1 "/usr/include/aarch64-linux-gnu/gnu/stubs-lp64.h" 1 3
# 9 "/usr/include/aarch64-linux-gnu/gnu/stubs.h" 2 3
# 564 "/usr/include/features.h" 2 3
# 26 "/usr/include/fcntl.h" 2 3



# 28 "/usr/include/fcntl.h" 3
extern "C" {


# 1 "/usr/include/aarch64-linux-gnu/bits/types.h" 1 3
# 27 "/usr/include/aarch64-linux-gnu/bits/types.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 28 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/timesize.h" 1 3
# 29 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3


typedef unsigned char __u_char;
typedef unsigned short int __u_short;
typedef unsigned int __u_int;
typedef unsigned long int __u_long;


typedef signed char __int8_t;
typedef unsigned char __uint8_t;
typedef signed short int __int16_t;
typedef unsigned short int __uint16_t;
typedef signed int __int32_t;
typedef unsigned int __uint32_t;

typedef signed long int __int64_t;
typedef unsigned long int __uint64_t;






typedef __int8_t __int_least8_t;
typedef __uint8_t __uint_least8_t;
typedef __int16_t __int_least16_t;
typedef __uint16_t __uint_least16_t;
typedef __int32_t __int_least32_t;
typedef __uint32_t __uint_least32_t;
typedef __int64_t __int_least64_t;
typedef __uint64_t __uint_least64_t;



typedef long int __quad_t;
typedef unsigned long int __u_quad_t;







typedef long int __intmax_t;
typedef unsigned long int __uintmax_t;
# 141 "/usr/include/aarch64-linux-gnu/bits/types.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/typesizes.h" 1 3
# 142 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/time64.h" 1 3
# 143 "/usr/include/aarch64-linux-gnu/bits/types.h" 2 3


typedef unsigned long int __dev_t;
typedef unsigned int __uid_t;
typedef unsigned int __gid_t;
typedef unsigned long int __ino_t;
typedef unsigned long int __ino64_t;
typedef unsigned int __mode_t;
typedef unsigned int __nlink_t;
typedef long int __off_t;
typedef long int __off64_t;
typedef int __pid_t;
typedef struct { int __val[2]; } __fsid_t;
typedef long int __clock_t;
typedef unsigned long int __rlim_t;
typedef unsigned long int __rlim64_t;
typedef unsigned int __id_t;
typedef long int __time_t;
typedef unsigned int __useconds_t;
typedef long int __suseconds_t;
typedef long int __suseconds64_t;

typedef int __daddr_t;
typedef int __key_t;


typedef int __clockid_t;


typedef void * __timer_t;


typedef int __blksize_t;




typedef long int __blkcnt_t;
typedef long int __blkcnt64_t;


typedef unsigned long int __fsblkcnt_t;
typedef unsigned long int __fsblkcnt64_t;


typedef unsigned long int __fsfilcnt_t;
typedef unsigned long int __fsfilcnt64_t;


typedef long int __fsword_t;

typedef long int __ssize_t;


typedef long int __syscall_slong_t;

typedef unsigned long int __syscall_ulong_t;



typedef __off64_t __loff_t;
typedef char *__caddr_t;


typedef long int __intptr_t;


typedef unsigned int __socklen_t;




typedef int __sig_atomic_t;
# 32 "/usr/include/fcntl.h" 2 3



# 1 "/usr/include/aarch64-linux-gnu/bits/fcntl.h" 1 3
# 34 "/usr/include/aarch64-linux-gnu/bits/fcntl.h" 3
struct flock
  {
    short int l_type;
    short int l_whence;
    __off_t l_start;
    __off_t l_len;
    __pid_t l_pid;
  };


struct flock64
  {
    short int l_type;
    short int l_whence;
    __off64_t l_start;
    __off64_t l_len;
    __pid_t l_pid;
  };



# 1 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 1 3
# 38 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/types/struct_iovec.h" 1 3
# 23 "/usr/include/aarch64-linux-gnu/bits/types/struct_iovec.h" 3
# 1 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stddef.h" 1 3
# 229 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stddef.h" 3
typedef long unsigned int size_t;
# 24 "/usr/include/aarch64-linux-gnu/bits/types/struct_iovec.h" 2 3


struct iovec
  {
    void *iov_base;
    size_t iov_len;
  };
# 39 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 2 3
# 270 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
enum __pid_type
  {
    F_OWNER_TID = 0,
    F_OWNER_PID,
    F_OWNER_PGRP,
    F_OWNER_GID = F_OWNER_PGRP
  };


struct f_owner_ex
  {
    enum __pid_type type;
    __pid_t pid;
  };
# 360 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
# 1 "/usr/include/linux/falloc.h" 1 3
# 361 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 2 3



struct file_handle
{
  unsigned int handle_bytes;
  int handle_type;

  unsigned char f_handle[0];
};
# 394 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
extern "C" {




extern __ssize_t readahead (int __fd, __off64_t __offset, size_t __count)
    noexcept (true);






extern int sync_file_range (int __fd, __off64_t __offset, __off64_t __count,
       unsigned int __flags);






extern __ssize_t vmsplice (int __fdout, const struct iovec *__iov,
      size_t __count, unsigned int __flags);





extern __ssize_t splice (int __fdin, __off64_t *__offin, int __fdout,
    __off64_t *__offout, size_t __len,
    unsigned int __flags);





extern __ssize_t tee (int __fdin, int __fdout, size_t __len,
        unsigned int __flags);






extern int fallocate (int __fd, int __mode, __off_t __offset, __off_t __len);
# 449 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
extern int fallocate64 (int __fd, int __mode, __off64_t __offset,
   __off64_t __len);




extern int name_to_handle_at (int __dfd, const char *__name,
         struct file_handle *__handle, int *__mnt_id,
         int __flags) noexcept (true);





extern int open_by_handle_at (int __mountdirfd, struct file_handle *__handle,
         int __flags);



# 1 "/usr/include/linux/openat2.h" 1 3




# 1 "/usr/include/linux/types.h" 1 3




# 1 "/usr/include/aarch64-linux-gnu/asm/types.h" 1 3
# 1 "/usr/include/asm-generic/types.h" 1 3






# 1 "/usr/include/asm-generic/int-ll64.h" 1 3
# 12 "/usr/include/asm-generic/int-ll64.h" 3
# 1 "/usr/include/aarch64-linux-gnu/asm/bitsperlong.h" 1 3
# 22 "/usr/include/aarch64-linux-gnu/asm/bitsperlong.h" 3
# 1 "/usr/include/asm-generic/bitsperlong.h" 1 3
# 23 "/usr/include/aarch64-linux-gnu/asm/bitsperlong.h" 2 3
# 13 "/usr/include/asm-generic/int-ll64.h" 2 3







typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;


__extension__ typedef __signed__ long long __s64;
__extension__ typedef unsigned long long __u64;
# 8 "/usr/include/asm-generic/types.h" 2 3
# 2 "/usr/include/aarch64-linux-gnu/asm/types.h" 2 3
# 6 "/usr/include/linux/types.h" 2 3



# 1 "/usr/include/linux/posix_types.h" 1 3




# 1 "/usr/include/linux/stddef.h" 1 3
# 6 "/usr/include/linux/posix_types.h" 2 3
# 25 "/usr/include/linux/posix_types.h" 3
typedef struct {
 unsigned long fds_bits[1024 / (8 * sizeof(long))];
} __kernel_fd_set;


typedef void (*__kernel_sighandler_t)(int);


typedef int __kernel_key_t;
typedef int __kernel_mqd_t;

# 1 "/usr/include/aarch64-linux-gnu/asm/posix_types.h" 1 3




typedef unsigned short __kernel_old_uid_t;
typedef unsigned short __kernel_old_gid_t;


# 1 "/usr/include/asm-generic/posix_types.h" 1 3
# 15 "/usr/include/asm-generic/posix_types.h" 3
typedef long __kernel_long_t;
typedef unsigned long __kernel_ulong_t;



typedef __kernel_ulong_t __kernel_ino_t;



typedef unsigned int __kernel_mode_t;



typedef int __kernel_pid_t;



typedef int __kernel_ipc_pid_t;



typedef unsigned int __kernel_uid_t;
typedef unsigned int __kernel_gid_t;



typedef __kernel_long_t __kernel_suseconds_t;



typedef int __kernel_daddr_t;



typedef unsigned int __kernel_uid32_t;
typedef unsigned int __kernel_gid32_t;
# 59 "/usr/include/asm-generic/posix_types.h" 3
typedef unsigned int __kernel_old_dev_t;
# 72 "/usr/include/asm-generic/posix_types.h" 3
typedef __kernel_ulong_t __kernel_size_t;
typedef __kernel_long_t __kernel_ssize_t;
typedef __kernel_long_t __kernel_ptrdiff_t;




typedef struct {
 int val[2];
} __kernel_fsid_t;





typedef __kernel_long_t __kernel_off_t;
typedef long long __kernel_loff_t;
typedef unsigned long long __kernel_uoff_t;
typedef __kernel_long_t __kernel_old_time_t;
typedef __kernel_long_t __kernel_time_t;
typedef long long __kernel_time64_t;
typedef __kernel_long_t __kernel_clock_t;
typedef int __kernel_timer_t;
typedef int __kernel_clockid_t;
typedef char * __kernel_caddr_t;
typedef unsigned short __kernel_uid16_t;
typedef unsigned short __kernel_gid16_t;
# 10 "/usr/include/aarch64-linux-gnu/asm/posix_types.h" 2 3
# 37 "/usr/include/linux/posix_types.h" 2 3
# 10 "/usr/include/linux/types.h" 2 3


typedef __signed__ __int128 __s128 __attribute__((aligned(16)));
typedef unsigned __int128 __u128 __attribute__((aligned(16)));
# 31 "/usr/include/linux/types.h" 3
typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

typedef __u16 __sum16;
typedef __u32 __wsum;
# 55 "/usr/include/linux/types.h" 3
typedef unsigned __poll_t;
# 6 "/usr/include/linux/openat2.h" 2 3
# 19 "/usr/include/linux/openat2.h" 3
struct open_how {
 __u64 flags;
 __u64 mode;
 __u64 resolve;
};
# 469 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 2 3




# 1 "/usr/include/aarch64-linux-gnu/bits/openat2.h" 1 3
# 474 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 2 3
# 484 "/usr/include/aarch64-linux-gnu/bits/fcntl-linux.h" 3
extern int openat2 (int __dfd, const char * __filename,
      const struct open_how * __how,
      long unsigned int __usize)
     __attribute__ ((__nonnull__ (2, 3)));







}
# 56 "/usr/include/aarch64-linux-gnu/bits/fcntl.h" 2 3
# 36 "/usr/include/fcntl.h" 2 3
# 50 "/usr/include/fcntl.h" 3
typedef __mode_t mode_t;





typedef __off_t off_t;







typedef __off64_t off64_t;




typedef __pid_t pid_t;





# 1 "/usr/include/aarch64-linux-gnu/bits/types/struct_timespec.h" 1 3





# 1 "/usr/include/aarch64-linux-gnu/bits/endian.h" 1 3
# 35 "/usr/include/aarch64-linux-gnu/bits/endian.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/endianness.h" 1 3
# 36 "/usr/include/aarch64-linux-gnu/bits/endian.h" 2 3
# 7 "/usr/include/aarch64-linux-gnu/bits/types/struct_timespec.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/types/time_t.h" 1 3
# 10 "/usr/include/aarch64-linux-gnu/bits/types/time_t.h" 3
typedef __time_t time_t;
# 8 "/usr/include/aarch64-linux-gnu/bits/types/struct_timespec.h" 2 3



struct timespec
{



  __time_t tv_sec;




  __syscall_slong_t tv_nsec;
# 31 "/usr/include/aarch64-linux-gnu/bits/types/struct_timespec.h" 3
};
# 76 "/usr/include/fcntl.h" 2 3


# 1 "/usr/include/aarch64-linux-gnu/bits/stat.h" 1 3
# 25 "/usr/include/aarch64-linux-gnu/bits/stat.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 1 3
# 27 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 28 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 2 3
# 44 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 3
struct stat
  {
    __dev_t st_dev;
    __ino_t st_ino;
    __mode_t st_mode;
    __nlink_t st_nlink;
    __uid_t st_uid;
    __gid_t st_gid;
    __dev_t st_rdev;
    __dev_t __pad1;
    __off_t st_size;
    __blksize_t st_blksize;
    int __pad2;
    __blkcnt_t st_blocks;







    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
# 79 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 3
    int __glibc_reserved[2];
  };




struct stat64
  {
    __dev_t st_dev;
    __ino64_t st_ino;
    __mode_t st_mode;
    __nlink_t st_nlink;
    __uid_t st_uid;
    __gid_t st_gid;
    __dev_t st_rdev;
    __dev_t __pad1;
    __off64_t st_size;
    __blksize_t st_blksize;
    int __pad2;
    __blkcnt64_t st_blocks;







    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
# 117 "/usr/include/aarch64-linux-gnu/bits/struct_stat.h" 3
    int __glibc_reserved[2];
  };
# 26 "/usr/include/aarch64-linux-gnu/bits/stat.h" 2 3
# 79 "/usr/include/fcntl.h" 2 3
# 177 "/usr/include/fcntl.h" 3
extern int fcntl (int __fd, int __cmd, ...);
# 186 "/usr/include/fcntl.h" 3
extern int fcntl64 (int __fd, int __cmd, ...);
# 209 "/usr/include/fcntl.h" 3
extern int open (const char *__file, int __oflag, ...) __attribute__ ((__nonnull__ (1)));
# 219 "/usr/include/fcntl.h" 3
extern int open64 (const char *__file, int __oflag, ...) __attribute__ ((__nonnull__ (1)));
# 233 "/usr/include/fcntl.h" 3
extern int openat (int __fd, const char *__file, int __oflag, ...)
     __attribute__ ((__nonnull__ (2)));
# 244 "/usr/include/fcntl.h" 3
extern int openat64 (int __fd, const char *__file, int __oflag, ...)
     __attribute__ ((__nonnull__ (2)));
# 255 "/usr/include/fcntl.h" 3
extern int creat (const char *__file, mode_t __mode) __attribute__ ((__nonnull__ (1)));
# 265 "/usr/include/fcntl.h" 3
extern int creat64 (const char *__file, mode_t __mode) __attribute__ ((__nonnull__ (1)));
# 284 "/usr/include/fcntl.h" 3
extern int lockf (int __fd, int __cmd, off_t __len) ;
# 294 "/usr/include/fcntl.h" 3
extern int lockf64 (int __fd, int __cmd, off64_t __len) ;







extern int posix_fadvise (int __fd, off_t __offset, off_t __len,
     int __advise) noexcept (true);
# 314 "/usr/include/fcntl.h" 3
extern int posix_fadvise64 (int __fd, off64_t __offset, off64_t __len,
       int __advise) noexcept (true);
# 324 "/usr/include/fcntl.h" 3
extern int posix_fallocate (int __fd, off_t __offset, off_t __len);
# 335 "/usr/include/fcntl.h" 3
extern int posix_fallocate64 (int __fd, off64_t __offset, off64_t __len);
# 345 "/usr/include/fcntl.h" 3
}
# 3 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 2
# 1 "/usr/include/unistd.h" 1 3
# 27 "/usr/include/unistd.h" 3
extern "C" {
# 202 "/usr/include/unistd.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/posix_opt.h" 1 3
# 203 "/usr/include/unistd.h" 2 3



# 1 "/usr/include/aarch64-linux-gnu/bits/environments.h" 1 3
# 22 "/usr/include/aarch64-linux-gnu/bits/environments.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/wordsize.h" 1 3
# 23 "/usr/include/aarch64-linux-gnu/bits/environments.h" 2 3
# 207 "/usr/include/unistd.h" 2 3
# 220 "/usr/include/unistd.h" 3
typedef __ssize_t ssize_t;





# 1 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stddef.h" 1 3
# 227 "/usr/include/unistd.h" 2 3





typedef __gid_t gid_t;




typedef __uid_t uid_t;
# 255 "/usr/include/unistd.h" 3
typedef __useconds_t useconds_t;
# 267 "/usr/include/unistd.h" 3
typedef __intptr_t intptr_t;






typedef __socklen_t socklen_t;
# 287 "/usr/include/unistd.h" 3
extern int access (const char *__name, int __type) noexcept (true) __attribute__ ((__nonnull__ (1)));




extern int euidaccess (const char *__name, int __type)
     noexcept (true) __attribute__ ((__nonnull__ (1)));


extern int eaccess (const char *__name, int __type)
     noexcept (true) __attribute__ ((__nonnull__ (1)));


extern int execveat (int __fd, const char *__path, char *const __argv[],
                     char *const __envp[], int __flags)
    noexcept (true) __attribute__ ((__nonnull__ (2, 3)));






extern int faccessat (int __fd, const char *__file, int __type, int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (2))) ;
# 339 "/usr/include/unistd.h" 3
extern __off_t lseek (int __fd, __off_t __offset, int __whence) noexcept (true);
# 350 "/usr/include/unistd.h" 3
extern __off64_t lseek64 (int __fd, __off64_t __offset, int __whence)
     noexcept (true);






extern int close (int __fd);




extern void closefrom (int __lowfd) noexcept (true);







extern ssize_t read (int __fd, void *__buf, size_t __nbytes)
    __attribute__ ((__access__ (__write_only__, 2, 3)));





extern ssize_t write (int __fd, const void *__buf, size_t __n)
    __attribute__ ((__access__ (__read_only__, 2, 3)));
# 389 "/usr/include/unistd.h" 3
extern ssize_t pread (int __fd, void *__buf, size_t __nbytes,
        __off_t __offset)
    __attribute__ ((__access__ (__write_only__, 2, 3)));






extern ssize_t pwrite (int __fd, const void *__buf, size_t __n,
         __off_t __offset)
    __attribute__ ((__access__ (__read_only__, 2, 3)));
# 422 "/usr/include/unistd.h" 3
extern ssize_t pread64 (int __fd, void *__buf, size_t __nbytes,
   __off64_t __offset)
    __attribute__ ((__access__ (__write_only__, 2, 3)));


extern ssize_t pwrite64 (int __fd, const void *__buf, size_t __n,
    __off64_t __offset)
    __attribute__ ((__access__ (__read_only__, 2, 3)));







extern int pipe (int __pipedes[2]) noexcept (true) ;




extern int pipe2 (int __pipedes[2], int __flags) noexcept (true) ;
# 452 "/usr/include/unistd.h" 3
extern unsigned int alarm (unsigned int __seconds) noexcept (true);
# 464 "/usr/include/unistd.h" 3
extern unsigned int sleep (unsigned int __seconds);







extern __useconds_t ualarm (__useconds_t __value, __useconds_t __interval)
     noexcept (true);






extern int usleep (__useconds_t __useconds);
# 489 "/usr/include/unistd.h" 3
extern int pause (void);



extern int chown (const char *__file, __uid_t __owner, __gid_t __group)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;



extern int fchown (int __fd, __uid_t __owner, __gid_t __group) noexcept (true) ;




extern int lchown (const char *__file, __uid_t __owner, __gid_t __group)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;






extern int fchownat (int __fd, const char *__file, __uid_t __owner,
       __gid_t __group, int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (2))) ;



extern int chdir (const char *__path) noexcept (true) __attribute__ ((__nonnull__ (1))) ;



extern int fchdir (int __fd) noexcept (true) ;
# 531 "/usr/include/unistd.h" 3
extern char *getcwd (char *__buf, size_t __size) noexcept (true) ;





extern char *get_current_dir_name (void) noexcept (true);







extern char *getwd (char *__buf)
     noexcept (true) __attribute__ ((__nonnull__ (1))) __attribute__ ((__deprecated__))
    __attribute__ ((__access__ (__write_only__, 1)));




extern int dup (int __fd) noexcept (true) ;


extern int dup2 (int __fd, int __fd2) noexcept (true);




extern int dup3 (int __fd, int __fd2, int __flags) noexcept (true);



extern char **__environ;

extern char **environ;





extern int execve (const char *__path, char *const __argv[],
     char *const __envp[]) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern int fexecve (int __fd, char *const __argv[], char *const __envp[])
     noexcept (true) __attribute__ ((__nonnull__ (2)));




extern int execv (const char *__path, char *const __argv[])
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));



extern int execle (const char *__path, const char *__arg, ...)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));



extern int execl (const char *__path, const char *__arg, ...)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));



extern int execvp (const char *__file, char *const __argv[])
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern int execlp (const char *__file, const char *__arg, ...)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));




extern int execvpe (const char *__file, char *const __argv[],
      char *const __envp[])
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));





extern int nice (int __inc) noexcept (true) ;




extern void _exit (int __status) __attribute__ ((__noreturn__));





# 1 "/usr/include/aarch64-linux-gnu/bits/confname.h" 1 3
# 24 "/usr/include/aarch64-linux-gnu/bits/confname.h" 3
enum
  {
    _PC_LINK_MAX,

    _PC_MAX_CANON,

    _PC_MAX_INPUT,

    _PC_NAME_MAX,

    _PC_PATH_MAX,

    _PC_PIPE_BUF,

    _PC_CHOWN_RESTRICTED,

    _PC_NO_TRUNC,

    _PC_VDISABLE,

    _PC_SYNC_IO,

    _PC_ASYNC_IO,

    _PC_PRIO_IO,

    _PC_SOCK_MAXBUF,

    _PC_FILESIZEBITS,

    _PC_REC_INCR_XFER_SIZE,

    _PC_REC_MAX_XFER_SIZE,

    _PC_REC_MIN_XFER_SIZE,

    _PC_REC_XFER_ALIGN,

    _PC_ALLOC_SIZE_MIN,

    _PC_SYMLINK_MAX,

    _PC_2_SYMLINKS

  };


enum
  {
    _SC_ARG_MAX,

    _SC_CHILD_MAX,

    _SC_CLK_TCK,

    _SC_NGROUPS_MAX,

    _SC_OPEN_MAX,

    _SC_STREAM_MAX,

    _SC_TZNAME_MAX,

    _SC_JOB_CONTROL,

    _SC_SAVED_IDS,

    _SC_REALTIME_SIGNALS,

    _SC_PRIORITY_SCHEDULING,

    _SC_TIMERS,

    _SC_ASYNCHRONOUS_IO,

    _SC_PRIORITIZED_IO,

    _SC_SYNCHRONIZED_IO,

    _SC_FSYNC,

    _SC_MAPPED_FILES,

    _SC_MEMLOCK,

    _SC_MEMLOCK_RANGE,

    _SC_MEMORY_PROTECTION,

    _SC_MESSAGE_PASSING,

    _SC_SEMAPHORES,

    _SC_SHARED_MEMORY_OBJECTS,

    _SC_AIO_LISTIO_MAX,

    _SC_AIO_MAX,

    _SC_AIO_PRIO_DELTA_MAX,

    _SC_DELAYTIMER_MAX,

    _SC_MQ_OPEN_MAX,

    _SC_MQ_PRIO_MAX,

    _SC_VERSION,

    _SC_PAGESIZE,


    _SC_RTSIG_MAX,

    _SC_SEM_NSEMS_MAX,

    _SC_SEM_VALUE_MAX,

    _SC_SIGQUEUE_MAX,

    _SC_TIMER_MAX,




    _SC_BC_BASE_MAX,

    _SC_BC_DIM_MAX,

    _SC_BC_SCALE_MAX,

    _SC_BC_STRING_MAX,

    _SC_COLL_WEIGHTS_MAX,

    _SC_EQUIV_CLASS_MAX,

    _SC_EXPR_NEST_MAX,

    _SC_LINE_MAX,

    _SC_RE_DUP_MAX,

    _SC_CHARCLASS_NAME_MAX,


    _SC_2_VERSION,

    _SC_2_C_BIND,

    _SC_2_C_DEV,

    _SC_2_FORT_DEV,

    _SC_2_FORT_RUN,

    _SC_2_SW_DEV,

    _SC_2_LOCALEDEF,


    _SC_PII,

    _SC_PII_XTI,

    _SC_PII_SOCKET,

    _SC_PII_INTERNET,

    _SC_PII_OSI,

    _SC_POLL,

    _SC_SELECT,

    _SC_UIO_MAXIOV,

    _SC_IOV_MAX = _SC_UIO_MAXIOV,

    _SC_PII_INTERNET_STREAM,

    _SC_PII_INTERNET_DGRAM,

    _SC_PII_OSI_COTS,

    _SC_PII_OSI_CLTS,

    _SC_PII_OSI_M,

    _SC_T_IOV_MAX,



    _SC_THREADS,

    _SC_THREAD_SAFE_FUNCTIONS,

    _SC_GETGR_R_SIZE_MAX,

    _SC_GETPW_R_SIZE_MAX,

    _SC_LOGIN_NAME_MAX,

    _SC_TTY_NAME_MAX,

    _SC_THREAD_DESTRUCTOR_ITERATIONS,

    _SC_THREAD_KEYS_MAX,

    _SC_THREAD_STACK_MIN,

    _SC_THREAD_THREADS_MAX,

    _SC_THREAD_ATTR_STACKADDR,

    _SC_THREAD_ATTR_STACKSIZE,

    _SC_THREAD_PRIORITY_SCHEDULING,

    _SC_THREAD_PRIO_INHERIT,

    _SC_THREAD_PRIO_PROTECT,

    _SC_THREAD_PROCESS_SHARED,


    _SC_NPROCESSORS_CONF,

    _SC_NPROCESSORS_ONLN,

    _SC_PHYS_PAGES,

    _SC_AVPHYS_PAGES,

    _SC_ATEXIT_MAX,

    _SC_PASS_MAX,


    _SC_XOPEN_VERSION,

    _SC_XOPEN_XCU_VERSION,

    _SC_XOPEN_UNIX,

    _SC_XOPEN_CRYPT,

    _SC_XOPEN_ENH_I18N,

    _SC_XOPEN_SHM,


    _SC_2_CHAR_TERM,

    _SC_2_C_VERSION,

    _SC_2_UPE,


    _SC_XOPEN_XPG2,

    _SC_XOPEN_XPG3,

    _SC_XOPEN_XPG4,


    _SC_CHAR_BIT,

    _SC_CHAR_MAX,

    _SC_CHAR_MIN,

    _SC_INT_MAX,

    _SC_INT_MIN,

    _SC_LONG_BIT,

    _SC_WORD_BIT,

    _SC_MB_LEN_MAX,

    _SC_NZERO,

    _SC_SSIZE_MAX,

    _SC_SCHAR_MAX,

    _SC_SCHAR_MIN,

    _SC_SHRT_MAX,

    _SC_SHRT_MIN,

    _SC_UCHAR_MAX,

    _SC_UINT_MAX,

    _SC_ULONG_MAX,

    _SC_USHRT_MAX,


    _SC_NL_ARGMAX,

    _SC_NL_LANGMAX,

    _SC_NL_MSGMAX,

    _SC_NL_NMAX,

    _SC_NL_SETMAX,

    _SC_NL_TEXTMAX,


    _SC_XBS5_ILP32_OFF32,

    _SC_XBS5_ILP32_OFFBIG,

    _SC_XBS5_LP64_OFF64,

    _SC_XBS5_LPBIG_OFFBIG,


    _SC_XOPEN_LEGACY,

    _SC_XOPEN_REALTIME,

    _SC_XOPEN_REALTIME_THREADS,


    _SC_ADVISORY_INFO,

    _SC_BARRIERS,

    _SC_BASE,

    _SC_C_LANG_SUPPORT,

    _SC_C_LANG_SUPPORT_R,

    _SC_CLOCK_SELECTION,

    _SC_CPUTIME,

    _SC_THREAD_CPUTIME,

    _SC_DEVICE_IO,

    _SC_DEVICE_SPECIFIC,

    _SC_DEVICE_SPECIFIC_R,

    _SC_FD_MGMT,

    _SC_FIFO,

    _SC_PIPE,

    _SC_FILE_ATTRIBUTES,

    _SC_FILE_LOCKING,

    _SC_FILE_SYSTEM,

    _SC_MONOTONIC_CLOCK,

    _SC_MULTI_PROCESS,

    _SC_SINGLE_PROCESS,

    _SC_NETWORKING,

    _SC_READER_WRITER_LOCKS,

    _SC_SPIN_LOCKS,

    _SC_REGEXP,

    _SC_REGEX_VERSION,

    _SC_SHELL,

    _SC_SIGNALS,

    _SC_SPAWN,

    _SC_SPORADIC_SERVER,

    _SC_THREAD_SPORADIC_SERVER,

    _SC_SYSTEM_DATABASE,

    _SC_SYSTEM_DATABASE_R,

    _SC_TIMEOUTS,

    _SC_TYPED_MEMORY_OBJECTS,

    _SC_USER_GROUPS,

    _SC_USER_GROUPS_R,

    _SC_2_PBS,

    _SC_2_PBS_ACCOUNTING,

    _SC_2_PBS_LOCATE,

    _SC_2_PBS_MESSAGE,

    _SC_2_PBS_TRACK,

    _SC_SYMLOOP_MAX,

    _SC_STREAMS,

    _SC_2_PBS_CHECKPOINT,


    _SC_V6_ILP32_OFF32,

    _SC_V6_ILP32_OFFBIG,

    _SC_V6_LP64_OFF64,

    _SC_V6_LPBIG_OFFBIG,


    _SC_HOST_NAME_MAX,

    _SC_TRACE,

    _SC_TRACE_EVENT_FILTER,

    _SC_TRACE_INHERIT,

    _SC_TRACE_LOG,


    _SC_LEVEL1_ICACHE_SIZE,

    _SC_LEVEL1_ICACHE_ASSOC,

    _SC_LEVEL1_ICACHE_LINESIZE,

    _SC_LEVEL1_DCACHE_SIZE,

    _SC_LEVEL1_DCACHE_ASSOC,

    _SC_LEVEL1_DCACHE_LINESIZE,

    _SC_LEVEL2_CACHE_SIZE,

    _SC_LEVEL2_CACHE_ASSOC,

    _SC_LEVEL2_CACHE_LINESIZE,

    _SC_LEVEL3_CACHE_SIZE,

    _SC_LEVEL3_CACHE_ASSOC,

    _SC_LEVEL3_CACHE_LINESIZE,

    _SC_LEVEL4_CACHE_SIZE,

    _SC_LEVEL4_CACHE_ASSOC,

    _SC_LEVEL4_CACHE_LINESIZE,



    _SC_IPV6 = _SC_LEVEL1_ICACHE_SIZE + 50,

    _SC_RAW_SOCKETS,


    _SC_V7_ILP32_OFF32,

    _SC_V7_ILP32_OFFBIG,

    _SC_V7_LP64_OFF64,

    _SC_V7_LPBIG_OFFBIG,


    _SC_SS_REPL_MAX,


    _SC_TRACE_EVENT_NAME_MAX,

    _SC_TRACE_NAME_MAX,

    _SC_TRACE_SYS_MAX,

    _SC_TRACE_USER_EVENT_MAX,


    _SC_XOPEN_STREAMS,


    _SC_THREAD_ROBUST_PRIO_INHERIT,

    _SC_THREAD_ROBUST_PRIO_PROTECT,


    _SC_MINSIGSTKSZ,


    _SC_SIGSTKSZ

  };


enum
  {
    _CS_PATH,


    _CS_V6_WIDTH_RESTRICTED_ENVS,



    _CS_GNU_LIBC_VERSION,

    _CS_GNU_LIBPTHREAD_VERSION,


    _CS_V5_WIDTH_RESTRICTED_ENVS,



    _CS_V7_WIDTH_RESTRICTED_ENVS,



    _CS_LFS_CFLAGS = 1000,

    _CS_LFS_LDFLAGS,

    _CS_LFS_LIBS,

    _CS_LFS_LINTFLAGS,

    _CS_LFS64_CFLAGS,

    _CS_LFS64_LDFLAGS,

    _CS_LFS64_LIBS,

    _CS_LFS64_LINTFLAGS,


    _CS_XBS5_ILP32_OFF32_CFLAGS = 1100,

    _CS_XBS5_ILP32_OFF32_LDFLAGS,

    _CS_XBS5_ILP32_OFF32_LIBS,

    _CS_XBS5_ILP32_OFF32_LINTFLAGS,

    _CS_XBS5_ILP32_OFFBIG_CFLAGS,

    _CS_XBS5_ILP32_OFFBIG_LDFLAGS,

    _CS_XBS5_ILP32_OFFBIG_LIBS,

    _CS_XBS5_ILP32_OFFBIG_LINTFLAGS,

    _CS_XBS5_LP64_OFF64_CFLAGS,

    _CS_XBS5_LP64_OFF64_LDFLAGS,

    _CS_XBS5_LP64_OFF64_LIBS,

    _CS_XBS5_LP64_OFF64_LINTFLAGS,

    _CS_XBS5_LPBIG_OFFBIG_CFLAGS,

    _CS_XBS5_LPBIG_OFFBIG_LDFLAGS,

    _CS_XBS5_LPBIG_OFFBIG_LIBS,

    _CS_XBS5_LPBIG_OFFBIG_LINTFLAGS,


    _CS_POSIX_V6_ILP32_OFF32_CFLAGS,

    _CS_POSIX_V6_ILP32_OFF32_LDFLAGS,

    _CS_POSIX_V6_ILP32_OFF32_LIBS,

    _CS_POSIX_V6_ILP32_OFF32_LINTFLAGS,

    _CS_POSIX_V6_ILP32_OFFBIG_CFLAGS,

    _CS_POSIX_V6_ILP32_OFFBIG_LDFLAGS,

    _CS_POSIX_V6_ILP32_OFFBIG_LIBS,

    _CS_POSIX_V6_ILP32_OFFBIG_LINTFLAGS,

    _CS_POSIX_V6_LP64_OFF64_CFLAGS,

    _CS_POSIX_V6_LP64_OFF64_LDFLAGS,

    _CS_POSIX_V6_LP64_OFF64_LIBS,

    _CS_POSIX_V6_LP64_OFF64_LINTFLAGS,

    _CS_POSIX_V6_LPBIG_OFFBIG_CFLAGS,

    _CS_POSIX_V6_LPBIG_OFFBIG_LDFLAGS,

    _CS_POSIX_V6_LPBIG_OFFBIG_LIBS,

    _CS_POSIX_V6_LPBIG_OFFBIG_LINTFLAGS,


    _CS_POSIX_V7_ILP32_OFF32_CFLAGS,

    _CS_POSIX_V7_ILP32_OFF32_LDFLAGS,

    _CS_POSIX_V7_ILP32_OFF32_LIBS,

    _CS_POSIX_V7_ILP32_OFF32_LINTFLAGS,

    _CS_POSIX_V7_ILP32_OFFBIG_CFLAGS,

    _CS_POSIX_V7_ILP32_OFFBIG_LDFLAGS,

    _CS_POSIX_V7_ILP32_OFFBIG_LIBS,

    _CS_POSIX_V7_ILP32_OFFBIG_LINTFLAGS,

    _CS_POSIX_V7_LP64_OFF64_CFLAGS,

    _CS_POSIX_V7_LP64_OFF64_LDFLAGS,

    _CS_POSIX_V7_LP64_OFF64_LIBS,

    _CS_POSIX_V7_LP64_OFF64_LINTFLAGS,

    _CS_POSIX_V7_LPBIG_OFFBIG_CFLAGS,

    _CS_POSIX_V7_LPBIG_OFFBIG_LDFLAGS,

    _CS_POSIX_V7_LPBIG_OFFBIG_LIBS,

    _CS_POSIX_V7_LPBIG_OFFBIG_LINTFLAGS,


    _CS_V6_ENV,

    _CS_V7_ENV

  };
# 631 "/usr/include/unistd.h" 2 3


extern long int pathconf (const char *__path, int __name)
     noexcept (true) __attribute__ ((__nonnull__ (1)));


extern long int fpathconf (int __fd, int __name) noexcept (true);


extern long int sysconf (int __name) noexcept (true);



extern size_t confstr (int __name, char *__buf, size_t __len) noexcept (true)
    __attribute__ ((__access__ (__write_only__, 2, 3)));




extern __pid_t getpid (void) noexcept (true);


extern __pid_t getppid (void) noexcept (true);


extern __pid_t getpgrp (void) noexcept (true);


extern __pid_t __getpgid (__pid_t __pid) noexcept (true);

extern __pid_t getpgid (__pid_t __pid) noexcept (true);






extern int setpgid (__pid_t __pid, __pid_t __pgid) noexcept (true);
# 682 "/usr/include/unistd.h" 3
extern int setpgrp (void) noexcept (true);






extern __pid_t setsid (void) noexcept (true);



extern __pid_t getsid (__pid_t __pid) noexcept (true);



extern __uid_t getuid (void) noexcept (true);


extern __uid_t geteuid (void) noexcept (true);


extern __gid_t getgid (void) noexcept (true);


extern __gid_t getegid (void) noexcept (true);




extern int getgroups (int __size, __gid_t __list[]) noexcept (true)
    __attribute__ ((__access__ (__write_only__, 2, 1)));


extern int group_member (__gid_t __gid) noexcept (true);






extern int setuid (__uid_t __uid) noexcept (true) ;




extern int setreuid (__uid_t __ruid, __uid_t __euid) noexcept (true) ;




extern int seteuid (__uid_t __uid) noexcept (true) ;






extern int setgid (__gid_t __gid) noexcept (true) ;




extern int setregid (__gid_t __rgid, __gid_t __egid) noexcept (true) ;




extern int setegid (__gid_t __gid) noexcept (true) ;





extern int getresuid (__uid_t *__ruid, __uid_t *__euid, __uid_t *__suid)
     noexcept (true);



extern int getresgid (__gid_t *__rgid, __gid_t *__egid, __gid_t *__sgid)
     noexcept (true);



extern int setresuid (__uid_t __ruid, __uid_t __euid, __uid_t __suid)
     noexcept (true) ;



extern int setresgid (__gid_t __rgid, __gid_t __egid, __gid_t __sgid)
     noexcept (true) ;






extern __pid_t fork (void) noexcept (true);







extern __pid_t vfork (void) noexcept (true);






extern __pid_t _Fork (void) noexcept (true);





extern char *ttyname (int __fd) noexcept (true);



extern int ttyname_r (int __fd, char *__buf, size_t __buflen)
     noexcept (true) __attribute__ ((__nonnull__ (2)))
     __attribute__ ((__access__ (__write_only__, 2, 3)));



extern int isatty (int __fd) noexcept (true);




extern int ttyslot (void) noexcept (true);




extern int link (const char *__from, const char *__to)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2))) ;




extern int linkat (int __fromfd, const char *__from, int __tofd,
     const char *__to, int __flags)
     noexcept (true) __attribute__ ((__nonnull__ (2, 4))) ;




extern int symlink (const char *__from, const char *__to)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2))) ;




extern ssize_t readlink (const char *__restrict __path,
    char *__restrict __buf, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)))
     __attribute__ ((__access__ (__write_only__, 2, 3)));





extern int symlinkat (const char *__from, int __tofd,
        const char *__to) noexcept (true) __attribute__ ((__nonnull__ (1, 3))) ;


extern ssize_t readlinkat (int __fd, const char *__restrict __path,
      char *__restrict __buf, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (2, 3)))
     __attribute__ ((__access__ (__write_only__, 3, 4)));



extern int unlink (const char *__name) noexcept (true) __attribute__ ((__nonnull__ (1)));



extern int unlinkat (int __fd, const char *__name, int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (2)));



extern int rmdir (const char *__path) noexcept (true) __attribute__ ((__nonnull__ (1)));



extern __pid_t tcgetpgrp (int __fd) noexcept (true);


extern int tcsetpgrp (int __fd, __pid_t __pgrp_id) noexcept (true);






extern char *getlogin (void);







extern int getlogin_r (char *__name, size_t __name_len) __attribute__ ((__nonnull__ (1)))
    __attribute__ ((__access__ (__write_only__, 1, 2)));




extern int setlogin (const char *__name) noexcept (true) __attribute__ ((__nonnull__ (1)));







# 1 "/usr/include/aarch64-linux-gnu/bits/getopt_posix.h" 1 3
# 27 "/usr/include/aarch64-linux-gnu/bits/getopt_posix.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/getopt_core.h" 1 3
# 28 "/usr/include/aarch64-linux-gnu/bits/getopt_core.h" 3
extern "C" {







extern char *optarg;
# 50 "/usr/include/aarch64-linux-gnu/bits/getopt_core.h" 3
extern int optind;




extern int opterr;



extern int optopt;
# 91 "/usr/include/aarch64-linux-gnu/bits/getopt_core.h" 3
extern int getopt (int ___argc, char *const *___argv, const char *__shortopts)
       noexcept (true) __attribute__ ((__nonnull__ (2, 3)));

}
# 28 "/usr/include/aarch64-linux-gnu/bits/getopt_posix.h" 2 3

extern "C" {
# 49 "/usr/include/aarch64-linux-gnu/bits/getopt_posix.h" 3
}
# 904 "/usr/include/unistd.h" 2 3







extern int gethostname (char *__name, size_t __len) noexcept (true) __attribute__ ((__nonnull__ (1)))
    __attribute__ ((__access__ (__write_only__, 1, 2)));






extern int sethostname (const char *__name, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (1))) __attribute__ ((__access__ (__read_only__, 1, 2)));



extern int sethostid (long int __id) noexcept (true) ;





extern int getdomainname (char *__name, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (1)))
     __attribute__ ((__access__ (__write_only__, 1, 2)));
extern int setdomainname (const char *__name, size_t __len)
     noexcept (true) __attribute__ ((__nonnull__ (1))) __attribute__ ((__access__ (__read_only__, 1, 2)));




extern int vhangup (void) noexcept (true);


extern int revoke (const char *__file) noexcept (true) __attribute__ ((__nonnull__ (1))) ;







extern int profil (unsigned short int *__sample_buffer, size_t __size,
     size_t __offset, unsigned int __scale)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int acct (const char *__name) noexcept (true);



extern char *getusershell (void) noexcept (true);
extern void endusershell (void) noexcept (true);
extern void setusershell (void) noexcept (true);





extern int daemon (int __nochdir, int __noclose) noexcept (true) ;






extern int chroot (const char *__path) noexcept (true) __attribute__ ((__nonnull__ (1))) ;



extern char *getpass (const char *__prompt) __attribute__ ((__nonnull__ (1)));







extern int fsync (int __fd);





extern int syncfs (int __fd) noexcept (true);






extern long int gethostid (void);


extern void sync (void) noexcept (true);





extern int getpagesize (void) noexcept (true) __attribute__ ((__const__));




extern int getdtablesize (void) noexcept (true);
# 1026 "/usr/include/unistd.h" 3
extern int truncate (const char *__file, __off_t __length)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;
# 1038 "/usr/include/unistd.h" 3
extern int truncate64 (const char *__file, __off64_t __length)
     noexcept (true) __attribute__ ((__nonnull__ (1))) ;
# 1049 "/usr/include/unistd.h" 3
extern int ftruncate (int __fd, __off_t __length) noexcept (true) ;
# 1059 "/usr/include/unistd.h" 3
extern int ftruncate64 (int __fd, __off64_t __length) noexcept (true) ;
# 1070 "/usr/include/unistd.h" 3
extern int brk (void *__addr) noexcept (true) ;





extern void *sbrk (intptr_t __delta) noexcept (true);
# 1091 "/usr/include/unistd.h" 3
extern long int syscall (long int __sysno, ...) noexcept (true);
# 1142 "/usr/include/unistd.h" 3
ssize_t copy_file_range (int __infd, __off64_t *__pinoff,
    int __outfd, __off64_t *__poutoff,
    size_t __length, unsigned int __flags);





extern int fdatasync (int __fildes);
# 1162 "/usr/include/unistd.h" 3
extern char *crypt (const char *__key, const char *__salt)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));







extern void swab (const void *__restrict __from, void *__restrict __to,
    ssize_t __n) noexcept (true) __attribute__ ((__nonnull__ (1, 2)))
    __attribute__ ((__access__ (__read_only__, 1, 3)))
    __attribute__ ((__access__ (__write_only__, 2, 3)));
# 1201 "/usr/include/unistd.h" 3
int getentropy (void *__buffer, size_t __length)
    __attribute__ ((__access__ (__write_only__, 1, 2)));
# 1211 "/usr/include/unistd.h" 3
extern int close_range (unsigned int __fd, unsigned int __max_fd,
   int __flags) noexcept (true);
# 1221 "/usr/include/unistd.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/unistd_ext.h" 1 3
# 34 "/usr/include/aarch64-linux-gnu/bits/unistd_ext.h" 3
extern __pid_t gettid (void) noexcept (true);



# 1 "/usr/include/linux/close_range.h" 1 3
# 39 "/usr/include/aarch64-linux-gnu/bits/unistd_ext.h" 2 3
# 1222 "/usr/include/unistd.h" 2 3

}
# 4 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 2
# 1 "/usr/include/aarch64-linux-gnu/sys/mman.h" 1 3
# 25 "/usr/include/aarch64-linux-gnu/sys/mman.h" 3
# 1 "/usr/local/lib/gcc/aarch64-unknown-linux-gnu/16.1.0/include/stddef.h" 1 3
# 26 "/usr/include/aarch64-linux-gnu/sys/mman.h" 2 3
# 41 "/usr/include/aarch64-linux-gnu/sys/mman.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/mman.h" 1 3
# 37 "/usr/include/aarch64-linux-gnu/bits/mman.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/mman-map-flags-generic.h" 1 3
# 38 "/usr/include/aarch64-linux-gnu/bits/mman.h" 2 3


# 1 "/usr/include/aarch64-linux-gnu/bits/mman-linux.h" 1 3
# 138 "/usr/include/aarch64-linux-gnu/bits/mman-linux.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/mman-shared.h" 1 3
# 50 "/usr/include/aarch64-linux-gnu/bits/mman-shared.h" 3
extern "C" {



int memfd_create (const char *__name, unsigned int __flags) noexcept (true);



int mlock2 (const void *__addr, size_t __length, unsigned int __flags) noexcept (true)
    __attribute__ ((__access__ (__none__, 1)));





int pkey_alloc (unsigned int __flags, unsigned int __access_restrictions) noexcept (true);



int pkey_set (int __key, unsigned int __access_restrictions) noexcept (true);



int pkey_get (int __key) noexcept (true);



int pkey_free (int __key) noexcept (true);



int pkey_mprotect (void *__addr, size_t __len, int __prot, int __pkey) noexcept (true);
# 91 "/usr/include/aarch64-linux-gnu/bits/mman-shared.h" 3
int mseal (void *__addr, size_t __len, unsigned long flags) noexcept (true);

}
# 139 "/usr/include/aarch64-linux-gnu/bits/mman-linux.h" 2 3
# 41 "/usr/include/aarch64-linux-gnu/bits/mman.h" 2 3
# 42 "/usr/include/aarch64-linux-gnu/sys/mman.h" 2 3




extern "C" {
# 57 "/usr/include/aarch64-linux-gnu/sys/mman.h" 3
extern void *mmap (void *__addr, size_t __len, int __prot,
     int __flags, int __fd, __off_t __offset) noexcept (true);
# 70 "/usr/include/aarch64-linux-gnu/sys/mman.h" 3
extern void *mmap64 (void *__addr, size_t __len, int __prot,
       int __flags, int __fd, __off64_t __offset) noexcept (true);




extern int munmap (void *__addr, size_t __len) noexcept (true);




extern int mprotect (void *__addr, size_t __len, int __prot) noexcept (true);







extern int msync (void *__addr, size_t __len, int __flags);




extern int madvise (void *__addr, size_t __len, int __advice) noexcept (true);



extern int posix_madvise (void *__addr, size_t __len, int __advice) noexcept (true);




extern int mlock (const void *__addr, size_t __len) noexcept (true)
    __attribute__ ((__access__ (__none__, 1)));


extern int munlock (const void *__addr, size_t __len) noexcept (true)
    __attribute__ ((__access__ (__none__, 1)));




extern int mlockall (int __flags) noexcept (true);



extern int munlockall (void) noexcept (true);







extern int mincore (void *__start, size_t __len, unsigned char *__vec)
     noexcept (true);
# 135 "/usr/include/aarch64-linux-gnu/sys/mman.h" 3
extern void *mremap (void *__addr, size_t __old_len, size_t __new_len,
       int __flags, ...) noexcept (true);



extern int remap_file_pages (void *__start, size_t __size, int __prot,
        size_t __pgoff, int __flags) noexcept (true);




extern int shm_open (const char *__name, int __oflag, mode_t __mode);


extern int shm_unlink (const char *__name);


# 1 "/usr/include/aarch64-linux-gnu/bits/mman_ext.h" 1 3
# 24 "/usr/include/aarch64-linux-gnu/bits/mman_ext.h" 3
struct iovec;
extern __ssize_t process_madvise (int __pid_fd, const struct iovec *__iov,
      size_t __count, int __advice,
      unsigned __flags)
  noexcept (true);

extern int process_mrelease (int pidfd, unsigned int flags) noexcept (true);
# 153 "/usr/include/aarch64-linux-gnu/sys/mman.h" 2 3

}
# 5 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 2
# 1 "/usr/include/aarch64-linux-gnu/sys/stat.h" 1 3
# 40 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
typedef __dev_t dev_t;
# 51 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
typedef __ino_t ino_t;
# 64 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
typedef __nlink_t nlink_t;
# 86 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
typedef __blkcnt_t blkcnt_t;







typedef __blksize_t blksize_t;




extern "C" {

# 1 "/usr/include/aarch64-linux-gnu/bits/stat.h" 1 3
# 102 "/usr/include/aarch64-linux-gnu/sys/stat.h" 2 3
# 205 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int stat (const char *__restrict __file,
   struct stat *__restrict __buf) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));



extern int fstat (int __fd, struct stat *__buf) noexcept (true) __attribute__ ((__nonnull__ (2)));
# 240 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int stat64 (const char *__restrict __file,
     struct stat64 *__restrict __buf) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
extern int fstat64 (int __fd, struct stat64 *__buf) noexcept (true) __attribute__ ((__nonnull__ (2)));
# 264 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int fstatat (int __fd, const char *__restrict __file,
      struct stat *__restrict __buf, int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (3)));
# 291 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int fstatat64 (int __fd, const char *__restrict __file,
        struct stat64 *__restrict __buf, int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (3)));
# 313 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int lstat (const char *__restrict __file,
    struct stat *__restrict __buf) noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
# 338 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int lstat64 (const char *__restrict __file,
      struct stat64 *__restrict __buf)
     noexcept (true) __attribute__ ((__nonnull__ (1, 2)));
# 352 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int chmod (const char *__file, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int lchmod (const char *__file, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (1)));




extern int fchmod (int __fd, __mode_t __mode) noexcept (true);





extern int fchmodat (int __fd, const char *__file, __mode_t __mode,
       int __flag)
     noexcept (true) __attribute__ ((__nonnull__ (2))) ;






extern __mode_t umask (__mode_t __mask) noexcept (true);




extern __mode_t getumask (void) noexcept (true);



extern int mkdir (const char *__path, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int mkdirat (int __fd, const char *__path, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (2)));






extern int mknod (const char *__path, __mode_t __mode, __dev_t __dev)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int mknodat (int __fd, const char *__path, __mode_t __mode,
      __dev_t __dev) noexcept (true) __attribute__ ((__nonnull__ (2)));





extern int mkfifo (const char *__path, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (1)));





extern int mkfifoat (int __fd, const char *__path, __mode_t __mode)
     noexcept (true) __attribute__ ((__nonnull__ (2)));






extern int utimensat (int __fd, const char *__path,
        const struct timespec __times[2],
        int __flags)
     noexcept (true);
# 452 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
extern int futimens (int __fd, const struct timespec __times[2]) noexcept (true);
# 465 "/usr/include/aarch64-linux-gnu/sys/stat.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/statx.h" 1 3
# 31 "/usr/include/aarch64-linux-gnu/bits/statx.h" 3
# 1 "/usr/include/linux/stat.h" 1 3
# 56 "/usr/include/linux/stat.h" 3
struct statx_timestamp {
 __s64 tv_sec;
 __u32 tv_nsec;
 __s32 __reserved;
};
# 99 "/usr/include/linux/stat.h" 3
struct statx {


 __u32 stx_mask;


 __u32 stx_blksize;


 __u64 stx_attributes;



 __u32 stx_nlink;


 __u32 stx_uid;


 __u32 stx_gid;


 __u16 stx_mode;
 __u16 __spare0[1];



 __u64 stx_ino;


 __u64 stx_size;


 __u64 stx_blocks;


 __u64 stx_attributes_mask;



 struct statx_timestamp stx_atime;


 struct statx_timestamp stx_btime;


 struct statx_timestamp stx_ctime;


 struct statx_timestamp stx_mtime;



 __u32 stx_rdev_major;
 __u32 stx_rdev_minor;


 __u32 stx_dev_major;
 __u32 stx_dev_minor;


 __u64 stx_mnt_id;


 __u32 stx_dio_mem_align;


 __u32 stx_dio_offset_align;



 __u64 stx_subvol;


 __u32 stx_atomic_write_unit_min;


 __u32 stx_atomic_write_unit_max;



 __u32 stx_atomic_write_segments_max;


 __u32 stx_dio_read_offset_align;


 __u32 stx_atomic_write_unit_max_opt;
 __u32 __spare2[1];


 __u64 __spare3[8];


};
# 32 "/usr/include/aarch64-linux-gnu/bits/statx.h" 2 3







# 1 "/usr/include/aarch64-linux-gnu/bits/statx-generic.h" 1 3
# 25 "/usr/include/aarch64-linux-gnu/bits/statx-generic.h" 3
# 1 "/usr/include/aarch64-linux-gnu/bits/types/struct_statx_timestamp.h" 1 3
# 26 "/usr/include/aarch64-linux-gnu/bits/statx-generic.h" 2 3
# 1 "/usr/include/aarch64-linux-gnu/bits/types/struct_statx.h" 1 3
# 27 "/usr/include/aarch64-linux-gnu/bits/statx-generic.h" 2 3
# 63 "/usr/include/aarch64-linux-gnu/bits/statx-generic.h" 3
extern "C" {


int statx (int __dirfd, const char *__restrict __path, int __flags,
           unsigned int __mask, struct statx *__restrict __buf)
  noexcept (true) __attribute__ ((__nonnull__ (5)));

}
# 40 "/usr/include/aarch64-linux-gnu/bits/statx.h" 2 3
# 466 "/usr/include/aarch64-linux-gnu/sys/stat.h" 2 3


}
# 6 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 2

# 6 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
module aura.compiler.cache;

namespace aura::compiler::cache {

using namespace aura::ast;






struct StringTable {
    std::vector<std::string> strings;
    std::pmr::vector<SymId> remapped;
};

static StringTable build_string_table(const FlatAST& flat, const StringPool& pool) {
    StringTable tbl;
    std::pmr::vector<SymId> remapped(flat.size(), INVALID_SYM);
    std::unordered_map<SymId, SymId> sym_to_idx;


    for (NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        SymId sid = v.sym_id;
        if (sid == INVALID_SYM) continue;

        auto it = sym_to_idx.find(sid);
        if (it != sym_to_idx.end()) {
            remapped[id] = it->second;
        } else {
            auto new_idx = static_cast<SymId>(tbl.strings.size());
            tbl.strings.push_back(std::string(pool.resolve(sid)));
            sym_to_idx[sid] = new_idx;
            remapped[id] = new_idx;
        }
    }
    tbl.remapped = std::move(remapped);
    return tbl;
}

bool write_cache(const std::string& path,
                 const FlatAST& flat,
                 const StringPool& pool,
                 NodeId root,
                 std::uint64_t source_mtime,
                 const aura::ir::IRModule* ir_mod) {

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    auto n = flat.size();
    if (n == 0) return false;


    auto stbl = build_string_table(flat, pool);


    std::vector<std::uint8_t> tags(n);
    std::vector<std::int64_t> int_vals(n, 0);
    std::vector<SymId> sym_ids(n, INVALID_SYM);
    std::vector<std::uint32_t> child_begins(n, 0);
    std::vector<std::uint32_t> child_counts(n, 0);
    std::vector<std::uint32_t> param_begins(n, 0);
    std::vector<std::uint32_t> param_counts(n, 0);
    std::vector<NodeId> child_data;
    std::vector<SymId> param_data;
    std::vector<std::uint32_t> lines(n, 0);
    std::vector<std::uint32_t> cols(n, 0);

    for (NodeId id = 0; id < n; ++id) {
        auto v = flat.get(id);
        tags[id] = static_cast<std::uint8_t>(v.tag);
        int_vals[id] = v.int_value;
        sym_ids[id] = stbl.remapped[id];
        lines[id] = v.line;
        cols[id] = v.col;

        child_begins[id] = static_cast<std::uint32_t>(child_data.size());
        child_counts[id] = static_cast<std::uint32_t>(v.children.size());
        child_data.insert(child_data.end(), v.children.begin(), v.children.end());

        param_begins[id] = static_cast<std::uint32_t>(param_data.size());
        param_counts[id] = static_cast<std::uint32_t>(v.params.size());
        param_data.insert(param_data.end(), v.params.begin(), v.params.end());
    }


    auto pad64 = [](std::uint64_t v) { return (v + 7) & ~7ull; };

    std::uint64_t off = sizeof(CacheHeader);
    std::uint64_t tags_off = off; off += pad64(n);
    std::uint64_t ints_off = off; off += pad64(n * 8);
    std::uint64_t syms_off = off; off += pad64(n * 4);
    std::uint64_t cb_off = off; off += pad64(n * 4);
    std::uint64_t cc_off = off; off += pad64(n * 4);
    std::uint64_t cd_off = off; off += pad64(child_data.size() * 4);
    std::uint64_t pb_off = off; off += pad64(n * 4);
    std::uint64_t pc_off = off; off += pad64(n * 4);
    std::uint64_t pd_off = off; off += pad64(param_data.size() * 4);
    std::uint64_t li_off = off; off += pad64(n * 4);
    std::uint64_t co_off = off; off += pad64(n * 4);







    std::uint64_t str_off = off;
    auto offsets_size = static_cast<std::uint64_t>(stbl.strings.size()) * 4;
    std::uint64_t str_data_start = 4 + offsets_size;
    std::vector<std::uint32_t> str_offsets;
    str_offsets.reserve(stbl.strings.size());
    {
        std::uint32_t cur = static_cast<std::uint32_t>(str_data_start);
        for (auto& s : stbl.strings) {
            str_offsets.push_back(cur);
            cur += 4 + static_cast<std::uint32_t>(s.size());
        }
    }


    CacheHeader header = {};
    std::memcpy(header.magic, "AURACACHE", 8);
    header.version = 3;
    header.num_nodes = static_cast<std::uint32_t>(n);
    header.num_strings = static_cast<std::uint32_t>(stbl.strings.size());
    header.content_hash = static_cast<std::uint64_t>(root);
    header.node_offset = sizeof(CacheHeader);
    header.string_offset = str_off;
    header.source_mtime = source_mtime;



    f.seekp(tags_off); f.write((const char*)tags.data(), n);
    f.seekp(ints_off); f.write((const char*)int_vals.data(), n * 8);
    f.seekp(syms_off); f.write((const char*)sym_ids.data(), n * 4);
    f.seekp(cb_off); f.write((const char*)child_begins.data(), n * 4);
    f.seekp(cc_off); f.write((const char*)child_counts.data(), n * 4);
    f.seekp(cd_off); f.write((const char*)child_data.data(), child_data.size() * 4);
    f.seekp(pb_off); f.write((const char*)param_begins.data(), n * 4);
    f.seekp(pc_off); f.write((const char*)param_counts.data(), n * 4);
    f.seekp(pd_off); f.write((const char*)param_data.data(), param_data.size() * 4);
    f.seekp(li_off); f.write((const char*)lines.data(), n * 4);
    f.seekp(co_off); f.write((const char*)cols.data(), n * 4);


    f.seekp(str_off);
    std::uint32_t num_strs = static_cast<std::uint32_t>(stbl.strings.size());
    f.write(reinterpret_cast<const char*>(&num_strs), 4);
    f.write(reinterpret_cast<const char*>(str_offsets.data()), offsets_size);
    for (auto& s : stbl.strings) {
        std::uint32_t len = static_cast<std::uint32_t>(s.size());
        f.write(reinterpret_cast<const char*>(&len), 4);
        f.write(s.data(), len);
    }


    auto ir_start = f.tellp();
    std::uint32_t num_functions_from_ir = 0;

    if (ir_mod) {
        auto write_str = [&](const std::string& s) {
            std::uint32_t len = static_cast<std::uint32_t>(s.size());
            f.write(reinterpret_cast<const char*>(&len), 4);
            f.write(s.data(), len);
        };


        std::uint32_t sp_sz = static_cast<std::uint32_t>(ir_mod->string_pool.size());
        f.write(reinterpret_cast<const char*>(&sp_sz), 4);
        for (auto& s : ir_mod->string_pool) write_str(s);


        std::uint32_t nf = static_cast<std::uint32_t>(ir_mod->functions.size());
        f.write(reinterpret_cast<const char*>(&nf), 4);
        for (auto& fn : ir_mod->functions) {
            f.write(reinterpret_cast<const char*>(&fn.id), 4);
            f.write(reinterpret_cast<const char*>(&fn.entry_block), 4);
            f.write(reinterpret_cast<const char*>(&fn.local_count), 4);
            f.write(reinterpret_cast<const char*>(&fn.arg_count), 4);
            write_str(fn.name);

            std::uint32_t np = static_cast<std::uint32_t>(fn.params.size());
            f.write(reinterpret_cast<const char*>(&np), 4);
            for (auto& p : fn.params) write_str(p);

            std::uint32_t nfv = static_cast<std::uint32_t>(fn.free_vars.size());
            f.write(reinterpret_cast<const char*>(&nfv), 4);
            for (auto& fv : fn.free_vars) write_str(fv);

            std::uint32_t nb = static_cast<std::uint32_t>(fn.blocks.size());
            f.write(reinterpret_cast<const char*>(&nb), 4);
            for (auto& blk : fn.blocks) {
                f.write(reinterpret_cast<const char*>(&blk.id), 4);
                std::uint32_t ni = static_cast<std::uint32_t>(blk.instructions.size());
                f.write(reinterpret_cast<const char*>(&ni), 4);
                for (auto& instr : blk.instructions) {
                    auto op = static_cast<std::uint8_t>(instr.opcode);
                    f.write(reinterpret_cast<const char*>(&op), 1);
                    f.write(reinterpret_cast<const char*>(instr.operands.data()), 16);
                    f.write(reinterpret_cast<const char*>(&instr.source_ast_node_id), 4);
                }
                std::uint32_t ns = static_cast<std::uint32_t>(blk.successors.size());
                f.write(reinterpret_cast<const char*>(&ns), 4);
                if (ns > 0)
                    f.write(reinterpret_cast<const char*>(blk.successors.data()), ns * 4);
            }
        }
        num_functions_from_ir = nf;
    }


    header.ir_offset = static_cast<std::uint64_t>(ir_start);
    header.num_functions = num_functions_from_ir;
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));

    f.close();
    return true;
}
# 246 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
void setup_pointers(MappedCache& cache) {
    auto n = cache.num_nodes_;
    auto* nd = static_cast<const std::uint8_t*>(cache.data_) + cache.header_->node_offset;
    std::size_t pos = 0;

    auto next_pad = [&](std::size_t sz) -> std::size_t {
        auto r = pos; pos += (sz + 7) & ~7ull; return r;
    };

    cache.tags_ = reinterpret_cast<const std::uint8_t*>(nd + next_pad(n * 1));
    cache.int_vals_ = reinterpret_cast<const std::int64_t*>(nd + next_pad(n * 8));
    cache.sym_ids_ = reinterpret_cast<const SymId*>(nd + next_pad(n * 4));
    cache.child_begins_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.child_counts_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_children = 0;
    for (std::size_t i = 0; i < n; ++i) total_children += cache.child_counts_[i];
    cache.child_data_ = reinterpret_cast<const NodeId*>(nd + next_pad(total_children * 4));

    cache.param_begins_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.param_counts_= reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));

    std::uint32_t total_params = 0;
    for (std::size_t i = 0; i < n; ++i) total_params += cache.param_counts_[i];
    cache.param_data_ = reinterpret_cast<const SymId*>(nd + next_pad(total_params * 4));

    cache.lines_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
    cache.cols_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));
}

MappedCache::MappedCache(MappedCache&& other) noexcept
    : data_(other.data_), file_size_(other.file_size_), header_(other.header_),
      num_nodes_(other.num_nodes_) {
    copy_pointers(other);
    other.data_ = nullptr; other.file_size_ = 0; other.header_ = nullptr;
}

MappedCache& MappedCache::operator=(MappedCache&& other) noexcept {
    if (this != &other) {
        if (valid()) munmap(data_, file_size_);
        data_ = other.data_; file_size_ = other.file_size_; header_ = other.header_;
        num_nodes_ = other.num_nodes_;
        copy_pointers(other);
        other.data_ = nullptr; other.file_size_ = 0; other.header_ = nullptr;
    }
    return *this;
}

void MappedCache::copy_pointers(const MappedCache& o) {
    tags_ = o.tags_; int_vals_ = o.int_vals_; sym_ids_ = o.sym_ids_;
    child_begins_ = o.child_begins_; child_counts_ = o.child_counts_;
    child_data_ = o.child_data_;
    param_begins_ = o.param_begins_; param_counts_ = o.param_counts_;
    param_data_ = o.param_data_;
    str_offsets_ = o.str_offsets_;
    str_data_base_ = o.str_data_base_;
    lines_ = o.lines_; cols_ = o.cols_;
    markers_ = o.markers_;
}

MappedCache::~MappedCache() {
    if (valid()) munmap(data_, file_size_);
}

NodeView MappedCache::get(NodeId id) const {
    if (!valid() || id >= num_nodes_) return {};
    return NodeView{
        .tag = static_cast<NodeTag>(tags_[id]),
        .int_value = int_vals_[id],
        .sym_id = sym_ids_[id],
        .line = id < num_nodes_ ? lines_[id] : 0,
        .col = id < num_nodes_ ? cols_[id] : 0,
        .children = std::span(child_data_ + child_begins_[id], child_counts_[id]),
        .params = std::span(param_data_ + param_begins_[id], param_counts_[id]),
        .marker = markers_ ? static_cast<SyntaxMarker>(markers_[id]) : SyntaxMarker::User,
    };
}

std::string_view MappedCache::resolve(SymId id) const {
    if (!valid() || !str_offsets_ || id >= header_->num_strings) return "";
    auto off = str_offsets_[id];
    auto* p = str_data_base_ + off;
    std::uint32_t len = 0;
    std::memcpy(&len, p, 4);
    return std::string_view(reinterpret_cast<const char*>(p + 4), len);
}

MappedCache open_cache(const std::string& path) {
    MappedCache cache;

    int fd = ::open(path.c_str(), 
# 336 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 3
                                 00
# 336 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
                                         );
    if (fd < 0) return cache;

    struct stat st;
    if (::fstat(fd, &st) < 0) { ::close(fd); return cache; }
    cache.file_size_ = static_cast<std::size_t>(st.st_size);

    void* data = ::mmap(nullptr, cache.file_size_, 
# 343 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 3
                                                  0x1
# 343 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
                                                           , 
# 343 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 3
                                                             0x02
# 343 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
                                                                        , fd, 0);
    ::close(fd);
    if (data == 
# 345 "/home/dev/code/aura/src/compiler/cache_impl.cpp" 3
               ((void *) -1)
# 345 "/home/dev/code/aura/src/compiler/cache_impl.cpp"
                         ) { cache.file_size_ = 0; return cache; }

    cache.data_ = data;
    auto* hdr = static_cast<const CacheHeader*>(data);
    if (std::memcmp(hdr->magic, "AURACACHE", 8) != 0 || hdr->version != 3) {
        ::munmap(data, cache.file_size_);
        cache.data_ = nullptr; cache.file_size_ = 0; return cache;
    }

    cache.header_ = hdr;
    cache.num_nodes_ = hdr->num_nodes;
    cache.root_ = static_cast<NodeId>(hdr->content_hash);
    setup_pointers(cache);



    auto* sp = static_cast<const std::uint8_t*>(data) + hdr->string_offset;
    cache.str_offsets_ = reinterpret_cast<const std::uint32_t*>(sp + 4);
    cache.str_data_base_ = sp;


    cache.ir_functions_.clear();
    cache.ir_string_pool_.clear();
    cache.ir_entry_function_id_ = 0;
    cache.has_ir_cache_ = false;

    if (hdr->ir_offset > 0 && hdr->num_functions > 0) {
        auto* irp = static_cast<const std::uint8_t*>(data) + hdr->ir_offset;

        auto read_str = [&]() -> std::string {
            std::uint32_t len = 0;
            std::memcpy(&len, irp, 4); irp += 4;
            std::string s(reinterpret_cast<const char*>(irp), len);
            irp += len;
            return s;
        };


        std::uint32_t sp_sz = 0;
        std::memcpy(&sp_sz, irp, 4); irp += 4;
        cache.ir_string_pool_.reserve(sp_sz);
        for (std::uint32_t i = 0; i < sp_sz; ++i)
            cache.ir_string_pool_.push_back(read_str());


        std::uint32_t nf = 0;
        std::memcpy(&nf, irp, 4); irp += 4;
        cache.ir_functions_.reserve(nf);
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            aura::ir::IRFunction fn;
            std::memcpy(&fn.id, irp, 4); irp += 4;
            std::memcpy(&fn.entry_block, irp, 4); irp += 4;
            std::memcpy(&fn.local_count, irp, 4); irp += 4;
            std::memcpy(&fn.arg_count, irp, 4); irp += 4;
            fn.name = read_str();

            std::uint32_t np = 0;
            std::memcpy(&np, irp, 4); irp += 4;
            fn.params.reserve(np);
            for (std::uint32_t pi = 0; pi < np; ++pi)
                fn.params.push_back(read_str());

            std::uint32_t nfv = 0;
            std::memcpy(&nfv, irp, 4); irp += 4;
            fn.free_vars.reserve(nfv);
            for (std::uint32_t fvi = 0; fvi < nfv; ++fvi)
                fn.free_vars.push_back(read_str());

            std::uint32_t nb = 0;
            std::memcpy(&nb, irp, 4); irp += 4;
            fn.blocks.reserve(nb);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                aura::ir::BasicBlock blk;
                std::memcpy(&blk.id, irp, 4); irp += 4;

                std::uint32_t ni = 0;
                std::memcpy(&ni, irp, 4); irp += 4;
                blk.instructions.reserve(ni);
                for (std::uint32_t ii = 0; ii < ni; ++ii) {
                    aura::ir::IRInstruction instr;
                    std::uint8_t op = 0;
                    std::memcpy(&op, irp, 1); irp += 1;
                    instr.opcode = static_cast<aura::ir::IROpcode>(op);
                    std::memcpy(instr.operands.data(), irp, 16); irp += 16;
                    std::memcpy(&instr.source_ast_node_id, irp, 4); irp += 4;
                    blk.instructions.push_back(std::move(instr));
                }

                std::uint32_t ns = 0;
                std::memcpy(&ns, irp, 4); irp += 4;
                blk.successors.resize(ns);
                if (ns > 0) {
                    std::memcpy(blk.successors.data(), irp, ns * 4);
                    irp += ns * 4;
                }
                fn.blocks.push_back(std::move(blk));
            }
            cache.ir_functions_.push_back(std::move(fn));
        }
        cache.ir_entry_function_id_ = 0;
        cache.has_ir_cache_ = true;
    }

    return cache;
}

bool remove_cache(const std::string& path) {
    return std::filesystem::remove(path);
}

}
