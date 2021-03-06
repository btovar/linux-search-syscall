/*
 *  linux/fs/read_write.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/slab.h> 
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/export.h>
#include <linux/syscalls.h>
#include <linux/pagemap.h>
#include <linux/splice.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/fs_struct.h>
#include "read_write.h"
#include "mount.h"

#include <asm/uaccess.h>
#include <asm/unistd.h>

const struct file_operations generic_ro_fops = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.aio_read	= generic_file_aio_read,
	.mmap		= generic_file_readonly_mmap,
	.splice_read	= generic_file_splice_read,
};

EXPORT_SYMBOL(generic_ro_fops);

static inline int unsigned_offsets(struct file *file)
{
	return file->f_mode & FMODE_UNSIGNED_OFFSET;
}

static loff_t lseek_execute(struct file *file, struct inode *inode,
		loff_t offset, loff_t maxsize)
{
	if (offset < 0 && !unsigned_offsets(file))
		return -EINVAL;
	if (offset > maxsize)
		return -EINVAL;

	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}
	return offset;
}

/**
 * generic_file_llseek_size - generic llseek implementation for regular files
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 * @size:	max size of file system
 *
 * This is a variant of generic_file_llseek that allows passing in a custom
 * file size.
 *
 * Synchronization:
 * SEEK_SET and SEEK_END are unsynchronized (but atomic on 64bit platforms)
 * SEEK_CUR is synchronized against other SEEK_CURs, but not read/writes.
 * read/writes behave like SEEK_SET against seeks.
 */
loff_t
generic_file_llseek_size(struct file *file, loff_t offset, int origin,
		loff_t maxsize)
{
	struct inode *inode = file->f_mapping->host;

	switch (origin) {
	case SEEK_END:
		offset += i_size_read(inode);
		break;
	case SEEK_CUR:
		/*
		 * Here we special-case the lseek(fd, 0, SEEK_CUR)
		 * position-querying operation.  Avoid rewriting the "same"
		 * f_pos value back to the file because a concurrent read(),
		 * write() or lseek() might have altered it
		 */
		if (offset == 0)
			return file->f_pos;
		/*
		 * f_lock protects against read/modify/write race with other
		 * SEEK_CURs. Note that parallel writes and reads behave
		 * like SEEK_SET.
		 */
		spin_lock(&file->f_lock);
		offset = lseek_execute(file, inode, file->f_pos + offset,
				       maxsize);
		spin_unlock(&file->f_lock);
		return offset;
	case SEEK_DATA:
		/*
		 * In the generic case the entire file is data, so as long as
		 * offset isn't at the end of the file then the offset is data.
		 */
		if (offset >= i_size_read(inode))
			return -ENXIO;
		break;
	case SEEK_HOLE:
		/*
		 * There is a virtual hole at the end of the file, so as long as
		 * offset isn't i_size or larger, return i_size.
		 */
		if (offset >= i_size_read(inode))
			return -ENXIO;
		offset = i_size_read(inode);
		break;
	}

	return lseek_execute(file, inode, offset, maxsize);
}
EXPORT_SYMBOL(generic_file_llseek_size);

/**
 * generic_file_llseek - generic llseek implementation for regular files
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * This is a generic implemenation of ->llseek useable for all normal local
 * filesystems.  It just updates the file offset to the value specified by
 * @offset and @origin under i_mutex.
 */
loff_t generic_file_llseek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_mapping->host;

	return generic_file_llseek_size(file, offset, origin,
					inode->i_sb->s_maxbytes);
}
EXPORT_SYMBOL(generic_file_llseek);

/**
 * noop_llseek - No Operation Performed llseek implementation
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * This is an implementation of ->llseek useable for the rare special case when
 * userspace expects the seek to succeed but the (device) file is actually not
 * able to perform the seek. In this case you use noop_llseek() instead of
 * falling back to the default implementation of ->llseek.
 */
loff_t noop_llseek(struct file *file, loff_t offset, int origin)
{
	return file->f_pos;
}
EXPORT_SYMBOL(noop_llseek);

loff_t no_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}
EXPORT_SYMBOL(no_llseek);

loff_t default_llseek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	loff_t retval;

	mutex_lock(&inode->i_mutex);
	switch (origin) {
		case SEEK_END:
			offset += i_size_read(inode);
			break;
		case SEEK_CUR:
			if (offset == 0) {
				retval = file->f_pos;
				goto out;
			}
			offset += file->f_pos;
			break;
		case SEEK_DATA:
			/*
			 * In the generic case the entire file is data, so as
			 * long as offset isn't at the end of the file then the
			 * offset is data.
			 */
			if (offset >= inode->i_size) {
				retval = -ENXIO;
				goto out;
			}
			break;
		case SEEK_HOLE:
			/*
			 * There is a virtual hole at the end of the file, so
			 * as long as offset isn't i_size or larger, return
			 * i_size.
			 */
			if (offset >= inode->i_size) {
				retval = -ENXIO;
				goto out;
			}
			offset = inode->i_size;
			break;
	}
	retval = -EINVAL;
	if (offset >= 0 || unsigned_offsets(file)) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
out:
	mutex_unlock(&inode->i_mutex);
	return retval;
}
EXPORT_SYMBOL(default_llseek);

loff_t vfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t (*fn)(struct file *, loff_t, int);

	fn = no_llseek;
	if (file->f_mode & FMODE_LSEEK) {
		if (file->f_op && file->f_op->llseek)
			fn = file->f_op->llseek;
	}
	return fn(file, offset, origin);
}
EXPORT_SYMBOL(vfs_llseek);

SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, origin)
{
	off_t retval;
	struct file * file;
	int fput_needed;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin <= SEEK_MAX) {
		loff_t res = vfs_llseek(file, offset, origin);
		retval = res;
		if (res != (loff_t)retval)
			retval = -EOVERFLOW;	/* LFS: should only happen on 32 bit platforms */
	}
	fput_light(file, fput_needed);
bad:
	return retval;
}

#ifdef __ARCH_WANT_SYS_LLSEEK
SYSCALL_DEFINE5(llseek, unsigned int, fd, unsigned long, offset_high,
		unsigned long, offset_low, loff_t __user *, result,
		unsigned int, origin)
{
	int retval;
	struct file * file;
	loff_t offset;
	int fput_needed;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin > SEEK_MAX)
		goto out_putf;

	offset = vfs_llseek(file, ((loff_t) offset_high << 32) | offset_low,
			origin);

	retval = (int)offset;
	if (offset >= 0) {
		retval = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;
	}
out_putf:
	fput_light(file, fput_needed);
bad:
	return retval;
}
#endif


/*
 * rw_verify_area doesn't like huge counts. We limit
 * them to something that fits in "int" so that others
 * won't have to do range checks all the time.
 */
int rw_verify_area(int read_write, struct file *file, loff_t *ppos, size_t count)
{
	struct inode *inode;
	loff_t pos;
	int retval = -EINVAL;

	inode = file->f_path.dentry->d_inode;
	if (unlikely((ssize_t) count < 0))
		return retval;
	pos = *ppos;
	if (unlikely(pos < 0)) {
		if (!unsigned_offsets(file))
			return retval;
		if (count >= -pos) /* both values are in 0..LLONG_MAX */
			return -EOVERFLOW;
	} else if (unlikely((loff_t) (pos + count) < 0)) {
		if (!unsigned_offsets(file))
			return retval;
	}

	if (unlikely(inode->i_flock && mandatory_lock(inode))) {
		retval = locks_mandatory_area(
			read_write == READ ? FLOCK_VERIFY_READ : FLOCK_VERIFY_WRITE,
			inode, file, pos, count);
		if (retval < 0)
			return retval;
	}
	retval = security_file_permission(file,
				read_write == READ ? MAY_READ : MAY_WRITE);
	if (retval)
		return retval;
	return count > MAX_RW_COUNT ? MAX_RW_COUNT : count;
}

static void wait_on_retry_sync_kiocb(struct kiocb *iocb)
{
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (!kiocbIsKicked(iocb))
		schedule();
	else
		kiocbClearKicked(iocb);
	__set_current_state(TASK_RUNNING);
}

ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = filp->f_op->aio_read(&kiocb, &iov, 1, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_read);

ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->read && !file->f_op->aio_read))
		return -EINVAL;
	if (unlikely(!access_ok(VERIFY_WRITE, buf, count)))
		return -EFAULT;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret >= 0) {
		count = ret;
		if (file->f_op->read)
			ret = file->f_op->read(file, buf, count, pos);
		else
			ret = do_sync_read(file, buf, count, pos);
		if (ret > 0) {
			fsnotify_access(file);
			add_rchar(current, ret);
		}
		inc_syscr(current);
	}

	return ret;
}

EXPORT_SYMBOL(vfs_read);

ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = len };
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = filp->f_op->aio_write(&kiocb, &iov, 1, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_write);

ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->write && !file->f_op->aio_write))
		return -EINVAL;
	if (unlikely(!access_ok(VERIFY_READ, buf, count)))
		return -EFAULT;

	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret >= 0) {
		count = ret;
		if (file->f_op->write)
			ret = file->f_op->write(file, buf, count, pos);
		else
			ret = do_sync_write(file, buf, count, pos);
		if (ret > 0) {
			fsnotify_modify(file);
			add_wchar(current, ret);
		}
		inc_syscw(current);
	}

	return ret;
}

EXPORT_SYMBOL(vfs_write);

static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;
}

static inline void file_pos_write(struct file *file, loff_t pos)
{
	file->f_pos = pos;
}

SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_read(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf,
		size_t, count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_write(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

SYSCALL_DEFINE(pread64)(unsigned int fd, char __user *buf,
			size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PREAD)
			ret = vfs_read(file, buf, count, &pos);
		fput_light(file, fput_needed);
	}

	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_pread64(long fd, long buf, long count, loff_t pos)
{
	return SYSC_pread64((unsigned int) fd, (char __user *) buf,
			    (size_t) count, pos);
}
SYSCALL_ALIAS(sys_pread64, SyS_pread64);
#endif

SYSCALL_DEFINE(pwrite64)(unsigned int fd, const char __user *buf,
			 size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PWRITE)  
			ret = vfs_write(file, buf, count, &pos);
		fput_light(file, fput_needed);
	}

	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_pwrite64(long fd, long buf, long count, loff_t pos)
{
	return SYSC_pwrite64((unsigned int) fd, (const char __user *) buf,
			     (size_t) count, pos);
}
SYSCALL_ALIAS(sys_pwrite64, SyS_pwrite64);
#endif

/*
 * Reduce an iovec's length in-place.  Return the resulting number of segments
 */
unsigned long iov_shorten(struct iovec *iov, unsigned long nr_segs, size_t to)
{
	unsigned long seg = 0;
	size_t len = 0;

	while (seg < nr_segs) {
		seg++;
		if (len + iov->iov_len >= to) {
			iov->iov_len = to - len;
			break;
		}
		len += iov->iov_len;
		iov++;
	}
	return seg;
}
EXPORT_SYMBOL(iov_shorten);

ssize_t do_sync_readv_writev(struct file *filp, const struct iovec *iov,
		unsigned long nr_segs, size_t len, loff_t *ppos, iov_fn_t fn)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = fn(&kiocb, iov, nr_segs, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (ret == -EIOCBQUEUED)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

/* Do it by hand, with file-ops */
ssize_t do_loop_readv_writev(struct file *filp, struct iovec *iov,
		unsigned long nr_segs, loff_t *ppos, io_fn_t fn)
{
	struct iovec *vector = iov;
	ssize_t ret = 0;

	while (nr_segs > 0) {
		void __user *base;
		size_t len;
		ssize_t nr;

		base = vector->iov_base;
		len = vector->iov_len;
		vector++;
		nr_segs--;

		nr = fn(filp, base, len, ppos);

		if (nr < 0) {
			if (!ret)
				ret = nr;
			break;
		}
		ret += nr;
		if (nr != len)
			break;
	}

	return ret;
}

/* A write operation does a read from user space and vice versa */
#define vrfy_dir(type) ((type) == READ ? VERIFY_WRITE : VERIFY_READ)

ssize_t rw_copy_check_uvector(int type, const struct iovec __user * uvector,
			      unsigned long nr_segs, unsigned long fast_segs,
			      struct iovec *fast_pointer,
			      struct iovec **ret_pointer,
			      int check_access)
{
	unsigned long seg;
	ssize_t ret;
	struct iovec *iov = fast_pointer;

	/*
	 * SuS says "The readv() function *may* fail if the iovcnt argument
	 * was less than or equal to 0, or greater than {IOV_MAX}.  Linux has
	 * traditionally returned zero for zero segments, so...
	 */
	if (nr_segs == 0) {
		ret = 0;
		goto out;
	}

	/*
	 * First get the "struct iovec" from user memory and
	 * verify all the pointers
	 */
	if (nr_segs > UIO_MAXIOV) {
		ret = -EINVAL;
		goto out;
	}
	if (nr_segs > fast_segs) {
		iov = kmalloc(nr_segs*sizeof(struct iovec), GFP_KERNEL);
		if (iov == NULL) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (copy_from_user(iov, uvector, nr_segs*sizeof(*uvector))) {
		ret = -EFAULT;
		goto out;
	}

	/*
	 * According to the Single Unix Specification we should return EINVAL
	 * if an element length is < 0 when cast to ssize_t or if the
	 * total length would overflow the ssize_t return value of the
	 * system call.
	 *
	 * Linux caps all read/write calls to MAX_RW_COUNT, and avoids the
	 * overflow case.
	 */
	ret = 0;
	for (seg = 0; seg < nr_segs; seg++) {
		void __user *buf = iov[seg].iov_base;
		ssize_t len = (ssize_t)iov[seg].iov_len;

		/* see if we we're about to use an invalid len or if
		 * it's about to overflow ssize_t */
		if (len < 0) {
			ret = -EINVAL;
			goto out;
		}
		if (check_access
		    && unlikely(!access_ok(vrfy_dir(type), buf, len))) {
			ret = -EFAULT;
			goto out;
		}
		if (len > MAX_RW_COUNT - ret) {
			len = MAX_RW_COUNT - ret;
			iov[seg].iov_len = len;
		}
		ret += len;
	}
out:
	*ret_pointer = iov;
	return ret;
}

static ssize_t do_readv_writev(int type, struct file *file,
			       const struct iovec __user * uvector,
			       unsigned long nr_segs, loff_t *pos)
{
	size_t tot_len;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	ssize_t ret;
	io_fn_t fn;
	iov_fn_t fnv;

	if (!file->f_op) {
		ret = -EINVAL;
		goto out;
	}

	ret = rw_copy_check_uvector(type, uvector, nr_segs,
				    ARRAY_SIZE(iovstack), iovstack, &iov, 1);
	if (ret <= 0)
		goto out;

	tot_len = ret;
	ret = rw_verify_area(type, file, pos, tot_len);
	if (ret < 0)
		goto out;

	fnv = NULL;
	if (type == READ) {
		fn = file->f_op->read;
		fnv = file->f_op->aio_read;
	} else {
		fn = (io_fn_t)file->f_op->write;
		fnv = file->f_op->aio_write;
	}

	if (fnv)
		ret = do_sync_readv_writev(file, iov, nr_segs, tot_len,
						pos, fnv);
	else
		ret = do_loop_readv_writev(file, iov, nr_segs, pos, fn);

out:
	if (iov != iovstack)
		kfree(iov);
	if ((ret + (type == READ)) > 0) {
		if (type == READ)
			fsnotify_access(file);
		else
			fsnotify_modify(file);
	}
	return ret;
}

ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
		  unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->aio_read && !file->f_op->read))
		return -EINVAL;

	return do_readv_writev(READ, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_readv);

ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
		   unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->aio_write && !file->f_op->write))
		return -EINVAL;

	return do_readv_writev(WRITE, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_writev);

SYSCALL_DEFINE3(readv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_readv(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

SYSCALL_DEFINE3(writev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_writev(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

static inline loff_t pos_from_hilo(unsigned long high, unsigned long low)
{
#define HALF_LONG_BITS (BITS_PER_LONG / 2)
	return (((loff_t)high << HALF_LONG_BITS) << HALF_LONG_BITS) | low;
}

SYSCALL_DEFINE5(preadv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PREAD)
			ret = vfs_readv(file, vec, vlen, &pos);
		fput_light(file, fput_needed);
	}

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

SYSCALL_DEFINE5(pwritev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PWRITE)
			ret = vfs_writev(file, vec, vlen, &pos);
		fput_light(file, fput_needed);
	}

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
			   size_t count, loff_t max)
{
	struct file * in_file, * out_file;
	struct inode * in_inode, * out_inode;
	loff_t pos;
	ssize_t retval;
	int fput_needed_in, fput_needed_out, fl;

	/*
	 * Get input file, and verify that it is ok..
	 */
	retval = -EBADF;
	in_file = fget_light(in_fd, &fput_needed_in);
	if (!in_file)
		goto out;
	if (!(in_file->f_mode & FMODE_READ))
		goto fput_in;
	retval = -ESPIPE;
	if (!ppos)
		ppos = &in_file->f_pos;
	else
		if (!(in_file->f_mode & FMODE_PREAD))
			goto fput_in;
	retval = rw_verify_area(READ, in_file, ppos, count);
	if (retval < 0)
		goto fput_in;
	count = retval;

	/*
	 * Get output file, and verify that it is ok..
	 */
	retval = -EBADF;
	out_file = fget_light(out_fd, &fput_needed_out);
	if (!out_file)
		goto fput_in;
	if (!(out_file->f_mode & FMODE_WRITE))
		goto fput_out;
	retval = -EINVAL;
	in_inode = in_file->f_path.dentry->d_inode;
	out_inode = out_file->f_path.dentry->d_inode;
	retval = rw_verify_area(WRITE, out_file, &out_file->f_pos, count);
	if (retval < 0)
		goto fput_out;
	count = retval;

	if (!max)
		max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

	pos = *ppos;
	if (unlikely(pos + count > max)) {
		retval = -EOVERFLOW;
		if (pos >= max)
			goto fput_out;
		count = max - pos;
	}

	fl = 0;
#if 0
	/*
	 * We need to debate whether we can enable this or not. The
	 * man page documents EAGAIN return for the output at least,
	 * and the application is arguably buggy if it doesn't expect
	 * EAGAIN on a non-blocking file descriptor.
	 */
	if (in_file->f_flags & O_NONBLOCK)
		fl = SPLICE_F_NONBLOCK;
#endif
	retval = do_splice_direct(in_file, ppos, out_file, count, fl);

	if (retval > 0) {
		add_rchar(current, retval);
		add_wchar(current, retval);
	}

	inc_syscr(current);
	inc_syscw(current);
	if (*ppos > max)
		retval = -EOVERFLOW;

fput_out:
	fput_light(out_file, fput_needed_out);
fput_in:
	fput_light(in_file, fput_needed_in);
out:
	return retval;
}

SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd, off_t __user *, offset, size_t, count)
{
	loff_t pos;
	off_t off;
	ssize_t ret;

	if (offset) {
		if (unlikely(get_user(off, offset)))
			return -EFAULT;
		pos = off;
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd, loff_t __user *, offset, size_t, count)
{
	loff_t pos;
	ssize_t ret;

	if (offset) {
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

/* search
   TODO
        * If top-level directory (base) is a link, then the output path is wrong. For pattern case.

	Patterns:
		* ? any char
		* * multiple chars
		* [] collating
		* | concat patterns
 */

enum search_matched {
  SEARCH_MATCH_FAILURE,
  SEARCH_MATCH_PARTIAL,
  SEARCH_MATCH_SUCCESS,
  SEARCH_MATCH_OVERFLOW,
};

static const char *strchrskip (const char *s, int c)
{
	s = strchr(s, c);
	if (s) s+=1;
	return s;
}

static int ispattern (const char *pattern)
{
	/* no forward slash at start indicates pattern */
	if (*pattern != '/') return 1;
	for (; *pattern; pattern += 1) {
		char c = *pattern;
		if (c == '*' || c == '?' || c == '|')
			return 1;
	}
	return 0;
}

#define is_filename(c)  ((c) != '/' && (c) != '\0')
static enum search_matched __match_pathname (int n, const char *pathname, const char *pattern, int flags)
{
	//printk("__match_pathname(%d, '%s', '%s', %d)\n", n, pathname, pattern, flags);
	if (n >= 8) return SEARCH_MATCH_OVERFLOW;
	while (1) {
		switch (pattern[0]) {
			case '\0':
				return pathname[0] == '\0' ? SEARCH_MATCH_SUCCESS : SEARCH_MATCH_FAILURE;
			case '*':
				do {
					enum search_matched result = __match_pathname(n+1, pathname, pattern+1, flags);
					if (result != SEARCH_MATCH_FAILURE)
						return result;
					pathname += 1;
				} while (is_filename(*(pathname-1)));
				return SEARCH_MATCH_FAILURE;
			case '?':
				if (is_filename(*pathname)) {
					enum search_matched result = __match_pathname(n+1, pathname+1, pattern+1, flags);
					if (result != SEARCH_MATCH_FAILURE)
						return result;
				}
				/* else ? is skipped */
				break;
			case '[':
				return SEARCH_MATCH_FAILURE;;
			case '|':
				if (pathname[0] == '\0')
					return SEARCH_MATCH_SUCCESS;
			case '/':
				if (pathname[0] == '\0')
					return SEARCH_MATCH_PARTIAL;
				/* else fall-through */
			default:
				if (pathname[0] == pattern[0]) {
					pathname += 1;
				} else {
					return SEARCH_MATCH_FAILURE;
				}
				break;
		}
		pattern += 1;
	}
}

/* needs to handle leading '/' for anchor; function should receive base as pathname */
static enum search_matched match_pathname (const char *pathname, const char *pattern, int flags)
{
	enum search_matched status = SEARCH_MATCH_FAILURE;
	const char *path = pathname;
	//printk("match_pathname(\"%s\", \"%s\", %d)\n", pathname, pattern, flags);
	for (; path; path = strchr(path+1, '/')) {
		const char *patt = pattern;
		for (; patt; patt = strchrskip(patt+1, '|')) {
			if (*patt == '/')
				status = __match_pathname(0, path, patt, flags);
			else
				status = __match_pathname(0, path+1, patt, flags);
			if (status != SEARCH_MATCH_FAILURE)
				return status;
		}
	}
	return status;
}

#define TREE_DEPTH  16
#define SEARCH_BUF  (PATH_MAX<<4)

#define SEARCH_STOPATFIRST (1<<0)
#define SEARCH_METADATA    (1<<1)
#define SEARCH_INCLUDEROOT (1<<2)
#define SEARCH_PERIOD      (1<<3)
#define SEARCH_R_OK        (1<<4)
#define SEARCH_W_OK        (1<<5)
#define SEARCH_X_OK        (1<<6)

static int isrecursive (const char *pattern)
{
	for (; pattern; pattern = strchrskip(pattern+1, '|')) {
		if (*pattern != '/')
			return 1;
	}
	return 0;
}

struct search_directory {
	/* normal stack variables */
	struct file *fp;
	char *dir;

	/* large state */
	char entries[SEARCH_BUF];
	char *next;

	/* matching an entry */
	char *entry;
	enum search_matched how;
	struct kstat stat;
	struct path path;
	char type;
};

struct dir_search {
	/* normal stack variables */
	int status;
	int results;

	char *paths;
	char *pattern;
	int flags;
	char __user *buf;
	char __user *next;
	size_t len;

	int isrecursive;
	int ispattern;
	size_t base;

	char path[PATH_MAX+1];

	/* result for copy_search_result with some room for stat */
	char result[PATH_MAX+1024];

	struct search_directory *dirs;

	/* used for fast PATH search */
	struct {
		struct kstat stat;
		struct path path[2];
	} psearch;
};

static int search_filldir (void *userdata, const char *name, int namelen, loff_t offset, u64 ino, unsigned int d_type)
{
	struct search_directory *ds = (struct search_directory *) userdata;

	if ((int)(SEARCH_BUF-(ds->next-ds->entries)) < namelen+3) {
		return -EINVAL; /* too many entries */
	}

	/* first char is 'd' for DT_DIR, 'o' for other */
	if (d_type == DT_DIR) {
		strcpy(ds->next, "d");
	} else {
		strcpy(ds->next, "o");
	}
	ds->next += 1;
	strcpy(ds->next, name);
	ds->next += namelen+1;
	*ds->next = '\0';

	return 0;
}

//static long cp_new_stat64(struct kstat *stat, struct stat64 __user *statbuf)
//{
//	struct stat64 tmp;
//
//	memset(&tmp, 0, sizeof(struct stat64));
//#ifdef CONFIG_MIPS
//	/* mips has weird padding, so we don't get 64 bits there */
//	if (!new_valid_dev(stat->dev) || !new_valid_dev(stat->rdev))
//		return -EOVERFLOW;
//	tmp.st_dev = new_encode_dev(stat->dev);
//	tmp.st_rdev = new_encode_dev(stat->rdev);
//#else
//	tmp.st_dev = huge_encode_dev(stat->dev);
//	tmp.st_rdev = huge_encode_dev(stat->rdev);
//#endif
//	tmp.st_ino = stat->ino;
//	if (sizeof(tmp.st_ino) < sizeof(stat->ino) && tmp.st_ino != stat->ino)
//		return -EOVERFLOW;
//#ifdef STAT64_HAS_BROKEN_ST_INO
//	tmp.__st_ino = stat->ino;
//#endif
//	tmp.st_mode = stat->mode;
//	tmp.st_nlink = stat->nlink;
//	tmp.st_uid = stat->uid;
//	tmp.st_gid = stat->gid;
//	tmp.st_atime = stat->atime.tv_sec;
//	tmp.st_atime_nsec = stat->atime.tv_nsec;
//	tmp.st_mtime = stat->mtime.tv_sec;
//	tmp.st_mtime_nsec = stat->mtime.tv_nsec;
//	tmp.st_ctime = stat->ctime.tv_sec;
//	tmp.st_ctime_nsec = stat->ctime.tv_nsec;
//	tmp.st_size = stat->size;
//	tmp.st_blocks = stat->blocks;
//	tmp.st_blksize = stat->blksize;
//	return copy_to_user(statbuf,&tmp,sizeof(tmp)) ? -EFAULT : 0;
//}

static int copy_search_result (struct dir_search *ds, char __user **buf, size_t *len, const char *path, const struct kstat *stat)
{
	size_t result_len;

	//printk("search: result `%s' ino:%ld mode:%x size:%d\n", path, (long int)stat->ino, (int)stat->mode, (int)stat->size);

	if (ds->flags & SEARCH_METADATA)
		sprintf(ds->result, "0|%s|%zd,%zd,%d,%zd,%d,%d,%zd,%zd,%zd,%zd,%zd,%zd,%zd|", 
			path,
			(ssize_t)huge_encode_dev(stat->dev),
			(ssize_t)stat->ino,
			(int)stat->mode,
			(ssize_t)stat->nlink,
			(int)stat->uid,
			(int)stat->gid,
			(ssize_t)huge_encode_dev(stat->rdev),
			(ssize_t)stat->size,
			(ssize_t)stat->atime.tv_sec,
			(ssize_t)stat->mtime.tv_sec,
			(ssize_t)stat->ctime.tv_sec,
			(ssize_t)stat->blksize,
			(ssize_t)stat->blocks);
	else
		sprintf(ds->result, "0|%s||", path);

	result_len = strlen(ds->result);
	ds->result[result_len] = '\0'; /* first NUL terminator (NOP) */
	ds->result[result_len+1] = '\0'; /* second NUL terminator */


	if (result_len+2 <= *len) { /* double NUL terminator */
		if (copy_to_user(*buf, ds->result, result_len+2))
			return -EFAULT;
        /* NUL does not delimit for parrot implementation, don't include +2 */
		*buf += result_len; *len -= result_len;
		return 0;
	} else {
		return -ERANGE;
	}
}

static int abspath (struct path *p, char *path)
{
	/* canonicalize path */
	char *s = d_absolute_path(p, path, PATH_MAX);
	if (IS_ERR(s))
		return PTR_ERR(s);
	memmove(path, s, strlen(s)+1);
	return 0;
}

static int search_directory (struct dir_search *ds, int n)
{
	//printk("search_directory(%p, %d, %zu, %p:\"%s\", %p:\"%s\", %d, %zu, %p)\n", ds, n, ds->base, ds->path, ds->path, ds->pattern, ds->pattern, ds->flags, ds->len, ds->buf);

	if (n >= TREE_DEPTH)
		return 0;

	ds->status = 0;

	ds->dirs[n].fp = filp_open(ds->path, O_DIRECTORY|O_RDONLY|O_LARGEFILE, 0);
	if (IS_ERR(ds->dirs[n].fp)) {
		ds->status = PTR_ERR(ds->dirs[n].fp);
		if (ds->status == -ENOENT || ds->status == -EPERM || ds->status == -EACCES || ds->status == -ENODEV)
			return 0;
		goto out;
	}

	ds->status = abspath(&ds->dirs[n].fp->f_path, ds->path); // expensive??
	if (ds->status)
		goto exit;

	if (ds->base == 0) /* not set? */
		ds->base = strlen(ds->path);

	/* Check if FS supports search natively */
	if (ds->dirs[n].fp->f_op && ds->dirs[n].fp->f_op->search) {

		/* Push search to FS driver */	
		char *pathbuf = kmalloc(PATH_MAX, GFP_TEMPORARY);
		struct file *filp = ds->dirs[n].fp;
		struct inode *inode = filp->f_mapping->host;
		struct mount *mnt = real_mount(filp->f_path.mnt);
		char *mount_real_path = dentry_path(mnt->mnt_mountpoint, pathbuf, PATH_MAX);
		char *rel_path = ds->path + strlen(mount_real_path);

		int driver_code = ds->dirs[n].fp->f_op->search(
			inode,
			mount_real_path,
			rel_path,
			ds->pattern,
			ds->flags,
			ds->next,
			ds->len
		);

		ds->results += driver_code;
		ds->next += driver_code;

		if (driver_code < 0) {
			ds->status = driver_code;
			goto out;
		}

		ds->len -= driver_code;

	} else {
		do {
			ds->dirs[n].next = ds->dirs[n].entries;
			ds->status = vfs_readdir(ds->dirs[n].fp, search_filldir, &ds->dirs[n]);
			if (ds->status)
				goto exit;

			if (ds->dirs[n].next > ds->dirs[n].entries) {
				ds->dirs[n].dir = ds->path+strlen(ds->path);
				for (ds->dirs[n].entry = ds->dirs[n].entries; *ds->dirs[n].entry; ds->dirs[n].entry = ds->dirs[n].entry+strlen(ds->dirs[n].entry)+1) {
					ds->dirs[n].type = *ds->dirs[n].entry;
					ds->dirs[n].entry += 1;

					strcat(ds->path, "/");
					strcat(ds->path, ds->dirs[n].entry);
					//printk("path: `%s' type: %c\n", ds->path, ds->dirs[n].type);

					ds->dirs[n].how = match_pathname(ds->path+ds->base, ds->pattern, ds->flags);
					if (ds->dirs[n].how == SEARCH_MATCH_SUCCESS) {
						//printk("matched `%s'\n", ds->path);
						ds->status = vfs_path_lookup(ds->dirs[n].fp->f_path.dentry, ds->dirs[n].fp->f_path.mnt, ds->dirs[n].entry, 0, &ds->dirs[n].path);
						if (ds->status)
							goto exit;
			    if (ds->flags & SEARCH_METADATA)
							ds->status = vfs_getattr(ds->dirs[n].path.mnt, ds->dirs[n].path.dentry, &ds->dirs[n].stat);
						else
							memset(&ds->dirs[0].stat, 0, sizeof(struct kstat));
						path_put(&ds->dirs[n].path);
						if (ds->status)
							goto exit;
						if (ds->flags & SEARCH_INCLUDEROOT)
							ds->status = copy_search_result(ds, &ds->next, &ds->len, ds->path, &ds->dirs[n].stat);
						else
							ds->status = copy_search_result(ds, &ds->next, &ds->len, ds->dirs[n].entry, &ds->dirs[n].stat);
						if (ds->status)
							goto exit;
						ds->results += 1;
						if (ds->flags & SEARCH_STOPATFIRST)
							goto exit;
					}
					if (ds->dirs[n].type == 'd' && strcmp(ds->dirs[n].entry, ".") != 0 && strcmp(ds->dirs[n].entry, "..") != 0 && (ds->dirs[n].how == SEARCH_MATCH_PARTIAL || ds->isrecursive)) {
						ds->status = search_directory(ds, n+1);
						if (ds->status)
							goto exit;
						/* check if we found something and STOPATFIRST is set */
						if (ds->results > 0 && ds->flags & SEARCH_STOPATFIRST)
							goto exit;
					} /* else SEARCH_MATCH_FAILURE */

					*ds->dirs[n].dir = '\0';
				}
			}
		} while (ds->dirs[n].next > ds->dirs[n].entries);
	}

exit:
	goto exitn;
exitn:
	filp_close(ds->dirs[n].fp, current->files); /* no need to check error? */
	goto out;

out:
	return ds->status;
}

SYSCALL_DEFINE5(search, const char __user *, paths, const char __user *, pattern, int, flags, char __user *, buf, size_t, len)
{
	//printk("paths: %s, pattern: %s, flags: %d\n", paths, pattern, flags);

	int status = 0;
	struct dir_search *ds;

	char *c;
	char *n;

	if (!access_ok(VERIFY_WRITE, buf, len)) {
		status = -EFAULT;
		goto exit0;
	}

	ds = kmalloc(sizeof(struct dir_search), GFP_KERNEL);
	if (!ds) {
		status = -ENOMEM;
		goto exit0;
	}

	ds->results = 0;

	ds->paths = getname(paths);
	if (IS_ERR(ds->paths)) {
		status = PTR_ERR(ds->paths);
		goto exit1;
	}

	ds->pattern = getname(pattern);
	if (IS_ERR(ds->pattern)) {
		status = PTR_ERR(ds->pattern);
		goto exit2;
	}

	ds->flags = flags;
	ds->buf = ds->next = buf;
	ds->len = len;
	ds->dirs = NULL;

	ds->isrecursive = isrecursive(ds->pattern);
	ds->ispattern = ispattern(ds->pattern);

	if (ds->ispattern) {
		ds->dirs = kmalloc(sizeof(struct search_directory)*TREE_DEPTH, GFP_KERNEL);
		if (!ds->dirs) {
			goto exit3;
		}
	} else {
		while (*ds->pattern == '/')
		  ds->pattern += 1; /* remove leading forward slashes */
	}

	//printk("search(%p:\"%s\", %p:\"%s\", %d, %zu, %p)\n", paths, ds->paths, pattern, ds->pattern, ds->flags, ds->len, ds->buf);

	n = ds->paths;
	while ((c = strsep(&n, "|")) != NULL) {
		strcpy(ds->path, c);

		if (ds->ispattern) {
			ds->base = 0; /* reset base to 0 as we are searching a new top-level directory */
			status = search_directory(ds, 0);
			if (status)
				goto exit;
			if (ds->results > 0 && ds->flags & SEARCH_STOPATFIRST)
				break;
		} else {
			/* TODO Really this should be merged into search_directory where each path component in the pattern is examined.
			   If there is no special characters in the pattern, then try to open the component directly and decompose the pattern.
			   Unfortunately this requires also putting pattern examination in search_directory when it's separate in match_pathname.
			   This will do for now...
			 */
			/* here we do another path_lookup and follow symlinks, we didn't the first time because we need the path with the final symlink */
			status = kern_path(ds->path, LOOKUP_FOLLOW, &ds->psearch.path[0]);
			if (status == -ENOENT)
				continue;
			else if (status)
				goto exit;
			ds->base = strlen(ds->path);
			strcat(ds->path, "/");
			strcat(ds->path, ds->pattern);
			status = vfs_path_lookup(ds->psearch.path[0].dentry, ds->psearch.path[0].mnt, ds->pattern, 0, &ds->psearch.path[1]);
			path_put(&ds->psearch.path[0]);
			if (status == -ENOENT)
				continue;
			else if (status)
				goto exit;
			if (ds->flags & SEARCH_METADATA)
				status = vfs_getattr(ds->psearch.path[1].mnt, ds->psearch.path[1].dentry, &ds->psearch.stat);
			else
				memset(&ds->psearch.stat, 0, sizeof(struct kstat));
			path_put(&ds->psearch.path[1]);
			if (status)
				goto exit;
			if (ds->flags & SEARCH_INCLUDEROOT)
				status = copy_search_result(ds, &ds->next, &ds->len, ds->path, &ds->psearch.stat);
			else
				status = copy_search_result(ds, &ds->next, &ds->len, ds->path+ds->base, &ds->psearch.stat);
			if (status)
				goto exit;
			ds->results += 1;
			if (ds->flags & SEARCH_STOPATFIRST)
				break;
		}
	}
	if (ds->buf != ds->next) {
		/* this is a sad hack because the '|' delimiter design
		 * makes programming this rather difficult.
		 * We remove the final '|' that was added.
		 */
		status = copy_to_user(ds->next-1, "\0\0", 2);
		if (status)
			goto exit;
	}
	status = ds->results;

	//printk("buffer: %s\n", buf);

exit:
	goto exitn;
exitn:
	kfree(ds->dirs);
exit3:
	putname(ds->pattern);
exit2:
	putname(ds->paths);
exit1:
	kfree(ds);
exit0:
    return status;
}
