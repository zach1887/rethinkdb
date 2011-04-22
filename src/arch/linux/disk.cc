#include <algorithm>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <unistd.h>
#include "arch/linux/system_event/eventfd.hpp"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "arch/linux/arch.hpp"
#include "config/args.hpp"
#include "utils2.hpp"
#include "arch/linux/coroutines.hpp"
#include "logger.hpp"
#include "arch/linux/disk/aio.hpp"
#include "arch/linux/disk/pool.hpp"
#include "arch/linux/disk/conflict_resolving.hpp"
#include "arch/linux/disk/stats.hpp"

// #define DEBUG_DUMP_WRITES 1

/* Disk manager object takes care of queueing operations, collecting statistics, preventing
conflicts, and actually sending them to the disk. Defined as an abstract class so that different
actual implementations can be swapped in at runtime. */

// TODO: If two files are on the same disk, should they share part of the disk manager?

struct linux_disk_manager_t {
    virtual ~linux_disk_manager_t() { }
    virtual void submit_write(fd_t fd, const void *buf, size_t count, size_t offset, linux_iocallback_t *cb) = 0;
    virtual void submit_read(fd_t fd, void *buf, size_t count, size_t offset, linux_iocallback_t *cb) = 0;
};

template<class backend_t>
struct linux_templated_disk_manager_t : public linux_disk_manager_t {

    backend_t backend;

    typedef conflict_resolving_diskmgr_t<typename backend_t::action_t> conflict_resolver_t;
    conflict_resolver_t conflict_resolver;

    typedef stats_diskmgr_t<typename conflict_resolver_t::action_t> stats_t;
    stats_t stats;

    struct action_t : public stats_t::action_t {
        linux_iocallback_t *cb;
    };

    linux_templated_disk_manager_t(linux_event_queue_t *queue) :
        backend(queue)
    {
        stats.submit_fun = boost::bind(&conflict_resolver_t::submit, &conflict_resolver, _1);
        conflict_resolver.submit_fun = boost::bind(&backend_t::submit, &backend, _1);
        backend.done_fun = boost::bind(&conflict_resolver_t::done, &conflict_resolver, _1);
        conflict_resolver.done_fun = boost::bind(&stats_t::done, &stats, _1);
        stats.done_fun = boost::bind(&linux_templated_disk_manager_t<backend_t>::done, this, _1);
    }

    void submit_write(fd_t fd, const void *buf, size_t count, size_t offset, linux_iocallback_t *cb) {
        action_t *a = new action_t;
        a->make_write(fd, buf, count, offset);
        a->cb = cb;
        stats.submit(a);
    }

    void submit_read(fd_t fd, void *buf, size_t count, size_t offset, linux_iocallback_t *cb) {
        action_t *a = new action_t;
        a->make_read(fd, buf, count, offset);
        a->cb = cb;
        stats.submit(a);
    };

    void done(typename stats_t::action_t *a) {
        action_t *a2 = static_cast<action_t *>(a);
        a2->cb->on_io_complete();
        delete a2;
    }
};

/* Disk file object */

linux_file_t::linux_file_t(const char *path, int mode, bool is_really_direct, const linux_io_backend_t io_backend)
    : fd(INVALID_FD)
{
    // Determine if it is a block device

    struct stat64 file_stat;
    bzero((void*)&file_stat, sizeof(file_stat)); // make valgrind happy
    int res = stat64(path, &file_stat);
    guarantee_err(res == 0 || errno == ENOENT, "Could not stat file '%s'", path);

    if (res == -1 && errno == ENOENT) {
        if (!(mode & mode_create)) {
            file_exists = false;
            return;
        }
        is_block = false;
    } else {
        is_block = S_ISBLK(file_stat.st_mode);
    }

    // Construct file flags

    int flags = O_CREAT | (is_really_direct ? O_DIRECT : 0) | O_LARGEFILE;

    if ((mode & mode_write) && (mode & mode_read)) flags |= O_RDWR;
    else if (mode & mode_write) flags |= O_WRONLY;
    else if (mode & mode_read) flags |= O_RDONLY;
    else crash("Bad file access mode.");


    // O_NOATIME requires owner or root privileges. This is a bit of a hack; we assume that
    // if we are opening a regular file, we are the owner, but if we are opening a block device,
    // we are not.
    if (!is_block) flags |= O_NOATIME;

    // Open the file

    fd.reset(open(path, flags, 0644));
    if (fd.get() == INVALID_FD)
        fail_due_to_user_error("Inaccessible database file: \"%s\": %s", path, strerror(errno));

    file_exists = true;

    // Determine the file size

    if (is_block) {
        res = ioctl(fd.get(), BLKGETSIZE64, &file_size);
        guarantee_err(res != -1, "Could not determine block device size");
    } else {
        off64_t size = lseek64(fd.get(), 0, SEEK_END);
        guarantee_err(size != -1, "Could not determine file size");
        res = lseek64(fd.get(), 0, SEEK_SET);
        guarantee_err(res != -1, "Could not reset file position");

        file_size = size;
    }

    // Construct a disk manager.
    if (linux_thread_pool_t::thread) {
        linux_event_queue_t *queue = &linux_thread_pool_t::thread->queue;
        if (io_backend == aio_native) {
            diskmgr.reset(new linux_templated_disk_manager_t<linux_diskmgr_aio_t>(queue));
        } else {
            diskmgr.reset(new linux_templated_disk_manager_t<pool_diskmgr_t>(queue));
        }
    }
}

bool linux_file_t::exists() {
    return file_exists;
}

bool linux_file_t::is_block_device() {
    return is_block;
}

size_t linux_file_t::get_size() {
    return file_size;
}

void linux_file_t::set_size(size_t size) {
    rassert(!is_block);
    int res = ftruncate(fd.get(), size);
    guarantee_err(res == 0, "Could not ftruncate()");
    file_size = size;
}

void linux_file_t::set_size_at_least(size_t size) {
    if (is_block) {
        rassert(file_size >= size);
    } else {
        /* Grow in large chunks at a time */
        if (file_size < size) {
            // TODO: we should make the growth rate of a db file
            // configurable.
            set_size(ceil_aligned(size, DEVICE_BLOCK_SIZE * 128));
        }
    }
}

bool linux_file_t::read_async(size_t offset, size_t length, void *buf, linux_iocallback_t *callback) {

    verify(offset, length, buf);
    diskmgr->submit_read(fd.get(), buf, length, offset, callback);

    return false;
}

bool linux_file_t::write_async(size_t offset, size_t length, const void *buf, linux_iocallback_t *callback) {

#ifdef DEBUG_DUMP_WRITES
    printf("--- WRITE BEGIN ---\n");
    print_backtrace(stdout);
    printf("\n");
    print_hd(buf, offset, length);
    printf("---- WRITE END ----\n\n");
#endif

    verify(offset, length, buf);
    diskmgr->submit_write(fd.get(), buf, length, offset, callback);

    return false;
}

void linux_file_t::read_blocking(size_t offset, size_t length, void *buf) {
    verify(offset, length, buf);
 tryagain:
    ssize_t res = pread(fd.get(), buf, length, offset);
    if (res == -1 && errno == EINTR) {
        goto tryagain;
    }
    rassert(size_t(res) == length, "Blocking read failed");
    (void)res;
}

void linux_file_t::write_blocking(size_t offset, size_t length, const void *buf) {
    verify(offset, length, buf);
 tryagain:
    ssize_t res = pwrite(fd.get(), buf, length, offset);
    if (res == -1 && errno == EINTR) {
        goto tryagain;
    }
    rassert(size_t(res) == length, "Blocking write failed");
    (void)res;
}

linux_file_t::~linux_file_t() {
    // scoped_fd_t's destructor takes care of close()ing the file
}

void linux_file_t::verify(UNUSED size_t offset, UNUSED size_t length, UNUSED const void *buf) {
    rassert(buf);
    rassert(offset + length <= file_size);
    rassert((intptr_t)buf % DEVICE_BLOCK_SIZE == 0);
    rassert(offset % DEVICE_BLOCK_SIZE == 0);
    rassert(length % DEVICE_BLOCK_SIZE == 0);
}
