// SPDX-License-Identifier: GPL-2.0
/* Persistent, dynamic GPU DMA-BUF imports for premapped MM requests.
 *
 * Locking: file lock protects handle ownership. dma_resv protects the map,
 * invalidation and busy transition. Never acquire file lock under dma_resv.
 * Hardware callbacks only publish atomic state and schedule work; all sleeping
 * cleanup and DMA fence signaling is performed outside the descq spinlock.
 *
 * Lifetime: registry -> registration -> file context -> cdev/module; an accepted
 * request additionally retains registration + SG + fence until proven complete.
 * Timeout never cancels or frees a QDMA request. Failed/short callbacks retain
 * everything, with an unsignaled fence, because they do not prove DMA quiescence.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ":persistent: " fmt
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "cdev.h"
#include "qdma_mod.h"
#include "qdma_persistent.h"
#include "qdma_persistent_logic.h"

DEFINE_MUTEX(qdma_cdev_lifecycle_mutex);
static struct workqueue_struct *completion_wq;
int qdma_persistent_init(void)
{
    completion_wq = alloc_workqueue("qdma_persistent", WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    return completion_wq ? 0 : -ENOMEM;
}
void qdma_persistent_shutdown(void)
{
    /* Last module references may be released at the tail of completion work
     * or fence-free RCU callbacks. Wait for their full return before any driver
     * teardown can free queue state or module text. Module refs prohibit unload
     * while requests, contexts, or unsignaled quarantine fences still exist. */
    rcu_barrier();
    if (completion_wq) {
        destroy_workqueue(completion_wq);
        completion_wq = NULL;
    }
}
static atomic64_t global_open = ATOMIC64_INIT(0);
static atomic64_t global_regs = ATOMIC64_INIT(0);
static atomic64_t global_active = ATOMIC64_INIT(0);
static atomic64_t global_quarantine = ATOMIC64_INIT(0);
static atomic64_t last_handle = ATOMIC64_INIT(0);
static LIST_HEAD(quarantined_requests);
static DEFINE_SPINLOCK(quarantine_lock);
static u64 allocate_handle(void)
{
    u64 old;
    do {
        old = (u64)atomic64_read(&last_handle);
        if (old == U64_MAX)
            return 0;
    } while ((u64)atomic64_cmpxchg(&last_handle, old, old + 1) != old);
    return old + 1;
}
static int counter_get(char *buffer, const struct kernel_param *kp)
{
    return scnprintf(buffer, PAGE_SIZE, "%lld\n", atomic64_read(kp->arg));
}
static const struct kernel_param_ops counter_ops = { .get = counter_get };
module_param_cb(persistent_open_files, &counter_ops, &global_open, 0444);
module_param_cb(persistent_registrations, &counter_ops, &global_regs, 0444);
module_param_cb(persistent_active_requests, &counter_ops, &global_active, 0444);
module_param_cb(persistent_quarantined_requests, &counter_ops, &global_quarantine, 0444);
MODULE_INFO(persistent_abi, QDMA_PERSISTENT_BUILD_ID);

struct persistent_registration {
    struct kref refs;
    struct list_head node;
    struct qdma_file_ctx *ctx;
    struct dma_buf *buf;
    struct dma_buf_attachment *attachment;
    struct sg_table *mapping;
    struct pci_dev *gpu;
    enum dma_data_direction dma_direction;
    u32 direction;
    u64 handle, length, generation, fence_context, sequence;
    bool stale;
    bool counted;
    atomic_t busy;
    atomic64_t mappings, invalidations, remaps, completed, quarantine;
};
struct persistent_fence {
    struct dma_fence base;
    spinlock_t lock;
};
struct persistent_request {
    struct qdma_request req;
    struct persistent_registration *reg;
    struct persistent_fence *fence;
    struct completion done;
    struct work_struct complete_work;
    struct list_head quarantine_node;
    refcount_t refs; /* ioctl waiter and accepted hardware request */
    atomic_t callback_seen;
    spinlock_t state_lock;
    bool finished, uncertain;
    u64 start_ns, duration_ns;
    unsigned int bytes;
    int error;
};

static void ctx_put(struct qdma_file_ctx *ctx)
{
    if (!refcount_dec_and_test(&ctx->refs))
        return;
    atomic_dec(&ctx->xcdev->persistent_users);
    atomic64_dec(&global_open);
    kfree(ctx);
    module_put(THIS_MODULE);
}

struct qdma_file_ctx *qdma_persistent_open(struct qdma_cdev *xcdev)
{
    struct qdma_file_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return ERR_PTR(-ENOMEM);
    if (!try_module_get(THIS_MODULE)) {
        kfree(ctx);
        return ERR_PTR(-ENODEV);
    }
    ctx->xcdev = xcdev;
    mutex_init(&ctx->lock);
    INIT_LIST_HEAD(&ctx->registrations);
    refcount_set(&ctx->refs, 1);
    atomic_inc(&xcdev->persistent_users);
    atomic64_inc(&global_open);
    return ctx;
}

static void unmap_locked(struct persistent_registration *reg)
{
    dma_resv_assert_held(reg->buf->resv);
    if (!reg->mapping)
        return;
    dma_buf_unmap_attachment(reg->attachment, reg->mapping, reg->dma_direction);
    reg->mapping = NULL;
}

static void registration_release(struct kref *ref)
{
    struct persistent_registration *reg = container_of(ref, typeof(*reg), refs);
    struct qdma_file_ctx *ctx = reg->ctx;
    /* Only registry removal plus absence of outstanding requests reaches here. */
    WARN_ON(atomic_read(&reg->busy));
    if (reg->attachment) {
        dma_resv_lock(reg->buf->resv, NULL);
        unmap_locked(reg);
        dma_resv_unlock(reg->buf->resv);
        dma_buf_detach(reg->buf, reg->attachment);
    }
    if (reg->buf)
        dma_buf_put(reg->buf);
    if (reg->gpu)
        pci_dev_put(reg->gpu);
    if (reg->counted) {
        atomic64_dec(&ctx->live);
        atomic64_dec(&global_regs);
    }
    kfree(reg);
    ctx_put(ctx);
}
static void registration_put(struct persistent_registration *reg)
{
    kref_put(&reg->refs, registration_release);
}
void qdma_persistent_close(struct qdma_file_ctx *ctx)
{
    struct persistent_registration *reg, *next;
    LIST_HEAD(retired);
    mutex_lock(&ctx->lock);
    ctx->closing = true;
    list_splice_init(&ctx->registrations, &retired);
    mutex_unlock(&ctx->lock);
    list_for_each_entry_safe(reg, next, &retired, node) {
        list_del_init(&reg->node);
        atomic64_inc(&ctx->unregistered);
        registration_put(reg);
    }
    ctx_put(ctx);
}
static struct persistent_registration *lookup(struct qdma_file_ctx *ctx, u64 handle)
{
    struct persistent_registration *reg, *found = NULL;
    mutex_lock(&ctx->lock);
    if (!ctx->closing && handle) {
        list_for_each_entry(reg, &ctx->registrations, node) {
            if (reg->handle == handle) {
                kref_get(&reg->refs);
                found = reg;
                break;
            }
        }
    }
    mutex_unlock(&ctx->lock);
    return found;
}

static void move_notify(struct dma_buf_attachment *attachment)
{
    struct persistent_registration *reg = attachment->importer_priv;
    dma_resv_assert_held(reg->buf->resv);
    reg->stale = true;
    atomic64_inc(&reg->invalidations);
    atomic64_inc(&reg->ctx->invalidations);
    /* Dynamic attachment mappings remain valid for pending DMA. Its published
     * READ/WRITE fence makes the exporter wait before relocating the resource.
     * Never wait here: the exporter already holds the reservation lock. */
    if (!atomic_read(&reg->busy))
        unmap_locked(reg);
}
static const struct dma_buf_attach_ops attach_ops = {
    .allow_peer2peer = true,
    .move_notify = move_notify,
};
static int wait_dependencies(struct persistent_registration *reg,
                             enum dma_resv_usage usage)
{
    long rv = dma_resv_wait_timeout(reg->buf->resv, usage, true,
                                    msecs_to_jiffies(10000));
    return rv > 0 ? 0 : (rv == 0 ? -ETIMEDOUT : (int)rv);
}
static int verify_vram(struct persistent_registration *reg)
{
    struct scatterlist *sg;
    u64 start = pci_bus_address(reg->gpu, 0);
    u64 size = pci_resource_len(reg->gpu, 0), end, total = 0;
    unsigned int i;
    if (!(pci_resource_flags(reg->gpu, 0) & IORESOURCE_MEM) || !size ||
        check_add_overflow(start, size, &end))
        return -ENODEV;
    for_each_sg(reg->mapping->sgl, sg, reg->mapping->nents, i) {
        u64 addr = sg_dma_address(sg), length = sg_dma_len(sg);
        if (!qdma_persistent_range_inside(addr, length, start, size) ||
            check_add_overflow(total, length, &total)) {
            pr_err("DMA-BUF is outside selected GPU BAR0; refusing non-VRAM mapping\n");
            return -EXDEV;
        }
    }
    return total >= reg->length ? 0 : -EINVAL;
}
static int map_locked(struct persistent_registration *reg)
{
    struct sg_table *sgt;
    int rv;
    dma_resv_assert_held(reg->buf->resv);
    if (reg->mapping && !reg->stale)
        return 0;
    if (atomic_read(&reg->busy))
        return -EBUSY;
    unmap_locked(reg);
    sgt = dma_buf_map_attachment(reg->attachment, reg->dma_direction);
    if (IS_ERR(sgt))
        return PTR_ERR(sgt);
    reg->mapping = sgt;
    /* Mapping itself may have triggered migration; those KERNEL fences must
     * complete even though the lifetime KFD BOOKKEEP fences must not be waited. */
    rv = wait_dependencies(reg, DMA_RESV_USAGE_KERNEL);
    if (!rv)
        rv = verify_vram(reg);
    if (rv) {
        unmap_locked(reg);
        return rv;
    }
    if (reg->generation) {
        atomic64_inc(&reg->remaps);
        atomic64_inc(&reg->ctx->remaps);
    }
    reg->generation++;
    reg->stale = false;
    atomic64_inc(&reg->mappings);
    atomic64_inc(&reg->ctx->mappings);
    return 0;
}
static int queue_ready(struct qdma_cdev *xcdev, u32 direction, unsigned long *qh)
{
    struct qdma_q_state state;
    char message[80];
    int rv;
    if (direction > 1 || !(xcdev->dir_init & (direction ? 1 : 2)))
        return -EINVAL;
    *qh = direction ? xcdev->h2c_qhndl : xcdev->c2h_qhndl;
    rv = qdma_get_queue_state(xcdev->xcb->xpdev->dev_hndl, *qh,
                              &state, message, sizeof(message));
    if (rv)
        return rv;
    if (state.st || state.q_type != (direction ? Q_H2C : Q_C2H))
        return -EOPNOTSUPP;
    return state.qstate == Q_STATE_ONLINE ? 0 : -ENXIO;
}
static bool valid_header(u32 size, u32 version, u32 expected)
{
    return size == expected && version == QDMA_PERSISTENT_ABI_VERSION;
}
static bool reserved_zero(const u64 *reserved, size_t count)
{
    while (count--)
        if (*reserved++)
            return false;
    return true;
}
static long register_buffer(struct qdma_file_ctx *ctx, void __user *user)
{
    struct qdma_persistent_register io;
    struct persistent_registration *reg;
    unsigned long qh;
    u64 start = ktime_get_ns();
    int rv;
    if (copy_from_user(&io, user, sizeof(io)))
        return -EFAULT;
    if (!valid_header(io.size, io.version, sizeof(io)) ||
        !reserved_zero(io.reserved, ARRAY_SIZE(io.reserved)) ||
        io.handle || io.registration_ns || io.mapping_generation ||
        io.dmabuf_fd < 0 || !io.length || io.length > U32_MAX ||
        io.gpu_domain > U16_MAX || io.gpu_bus > U8_MAX ||
        io.gpu_device > 31 || io.gpu_function > 7)
        return -EINVAL;
    if (atomic_read(&ctx->quarantined))
        return -EIO;
    if (atomic_read(&ctx->uncertain_waits))
        return -EBUSY;
    rv = queue_ready(ctx->xcdev, io.direction, &qh);
    if (rv)
        return rv;
    reg = kzalloc(sizeof(*reg), GFP_KERNEL);
    if (!reg)
        return -ENOMEM;
    kref_init(&reg->refs);
    INIT_LIST_HEAD(&reg->node);
    reg->ctx = ctx;
    refcount_inc(&ctx->refs);
    reg->length = io.length;
    reg->direction = io.direction;
    reg->dma_direction = io.direction ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    reg->fence_context = dma_fence_context_alloc(1);
    reg->gpu = pci_get_domain_bus_and_slot(io.gpu_domain, io.gpu_bus,
                                          PCI_DEVFN(io.gpu_device, io.gpu_function));
    if (!reg->gpu || reg->gpu->vendor != PCI_VENDOR_ID_ATI) {
        rv = -ENODEV;
        goto fail;
    }
    reg->buf = dma_buf_get(io.dmabuf_fd);
    if (IS_ERR(reg->buf)) {
        rv = PTR_ERR(reg->buf);
        reg->buf = NULL;
        goto fail;
    }
    if (io.length > reg->buf->size) {
        rv = -EINVAL;
        goto fail;
    }
    if (!dma_buf_is_dynamic(reg->buf)) {
        rv = -EOPNOTSUPP;
        goto fail;
    }
    reg->attachment = dma_buf_dynamic_attach(reg->buf,
        &ctx->xcdev->xcb->xpdev->pdev->dev, &attach_ops, reg);
    if (IS_ERR(reg->attachment)) {
        rv = PTR_ERR(reg->attachment);
        reg->attachment = NULL;
        goto fail;
    }
    rv = dma_resv_lock_interruptible(reg->buf->resv, NULL);
    if (rv)
        goto fail;
    rv = map_locked(reg);
    io.mapping_generation = reg->generation;
    dma_resv_unlock(reg->buf->resv);
    if (rv)
        goto fail;
    mutex_lock(&ctx->lock);
    if (ctx->closing) {
        rv = -EBADF;
        mutex_unlock(&ctx->lock);
        goto fail;
    }
    reg->handle = allocate_handle();
    if (!reg->handle) {
        rv = -EOVERFLOW;
        mutex_unlock(&ctx->lock);
        goto fail;
    }
    io.handle = reg->handle;
    io.registration_ns = ktime_get_ns() - start;
    /* Keep the handle unpublished if userspace cannot receive it. */
    if (copy_to_user(user, &io, sizeof(io))) {
        mutex_unlock(&ctx->lock);
        rv = -EFAULT;
        goto fail;
    }
    reg->counted = true;
    atomic64_inc(&ctx->registered);
    atomic64_inc(&ctx->live);
    atomic64_inc(&global_regs);
    list_add_tail(&reg->node, &ctx->registrations);
    mutex_unlock(&ctx->lock);
    return 0;
fail:
    registration_put(reg);
    return rv;
}

static const char *fence_driver(struct dma_fence *fence) { return "qdma-persistent"; }
static const char *fence_timeline(struct dma_fence *fence) { return "qdma-mm"; }
static void fence_free_rcu(struct rcu_head *head)
{
    struct dma_fence *base = container_of(head, struct dma_fence, rcu);
    struct persistent_fence *fence = container_of(base, typeof(*fence), base);
    kfree(fence);
    module_put(THIS_MODULE);
}
static void fence_release(struct dma_fence *base)
{
    /* Preserve the module until the RCU callback runs. Shutdown's rcu_barrier
     * then covers the callback's return after its last module_put. */
    call_rcu(&base->rcu, fence_free_rcu);
}
static const struct dma_fence_ops fence_ops = {
    .get_driver_name = fence_driver,
    .get_timeline_name = fence_timeline,
    .release = fence_release,
};
static struct persistent_fence *new_fence(struct persistent_registration *reg)
{
    struct persistent_fence *fence = kzalloc(sizeof(*fence), GFP_KERNEL);
    if (!fence)
        return NULL;
    if (!try_module_get(THIS_MODULE)) {
        kfree(fence);
        return NULL;
    }
    spin_lock_init(&fence->lock);
    dma_fence_init(&fence->base, &fence_ops, &fence->lock,
                   reg->fence_context, ++reg->sequence);
    return fence;
}
static void request_put(struct persistent_request *request)
{
    if (!refcount_dec_and_test(&request->refs))
        return;
    dma_fence_put(&request->fence->base);
    kfree(request->req.sgl);
    registration_put(request->reg);
    kfree(request);
}
static void complete_work(struct work_struct *work)
{
    struct persistent_request *request = container_of(work, typeof(*request), complete_work);
    struct persistent_registration *reg = request->reg;
    unsigned long flags;
    /* Only full, successful hardware callbacks schedule this worker. */
    dma_fence_signal(&request->fence->base);
    dma_resv_lock(reg->buf->resv, NULL);
    atomic_set(&reg->busy, 0);
    if (reg->stale)
        unmap_locked(reg);
    atomic64_dec(&reg->ctx->active);
    atomic64_dec(&global_active);
    atomic64_inc(&reg->completed);
    atomic64_inc(&reg->ctx->completed);
    dma_resv_unlock(reg->buf->resv);
    spin_lock_irqsave(&request->state_lock, flags);
    request->finished = true;
    if (request->uncertain)
        atomic_dec(&reg->ctx->uncertain_waits);
    spin_unlock_irqrestore(&request->state_lock, flags);
    complete_all(&request->done);
    request_put(request); /* hardware reference */
}
static int request_done(struct qdma_request *req, unsigned int bytes, int error)
{
    struct persistent_request *request = container_of(req, typeof(*request), req);
    struct persistent_registration *reg = request->reg;
    if (atomic_cmpxchg(&request->callback_seen, 0, 1))
        return 0;
    request->duration_ns = ktime_get_ns() - request->start_ns;
    request->bytes = bytes;
    request->error = qdma_persistent_full_completion(req->count, bytes, error) ?
                     0 : (error ? error : -EIO);
    if (request->error) {
        unsigned long flags;
        /* A queue-stop/error callback is not a hardware quiescence proof. */
        spin_lock_irqsave(&quarantine_lock, flags);
        list_add_tail(&request->quarantine_node, &quarantined_requests);
        spin_unlock_irqrestore(&quarantine_lock, flags);
        atomic_set(&reg->ctx->quarantined, 1);
        atomic64_inc(&reg->quarantine);
        atomic64_inc(&reg->ctx->quarantine);
        atomic64_inc(&global_quarantine);
        pr_err("quarantined handle %llu: completion error %d, bytes %u/%u; resources retained\n",
               reg->handle, request->error, bytes, req->count);
        complete_all(&request->done);
        return 0;
    }
    queue_work(completion_wq, &request->complete_work);
    return 0;
}
static int exact_sgl(struct persistent_registration *reg,
                      struct qdma_request *req, u32 length)
{
    struct scatterlist *sg;
    struct qdma_persistent_sg_cursor cursor = { .remaining = length };
    u32 used = 0;
    unsigned int i;
    req->sgl = kcalloc(reg->mapping->nents, sizeof(*req->sgl), GFP_KERNEL);
    if (!req->sgl)
        return -ENOMEM;
    for_each_sg(reg->mapping->sgl, sg, reg->mapping->nents, i) {
        struct qdma_sw_sg *out;
        u32 bytes;
        if (!cursor.remaining)
            break;
        bytes = qdma_persistent_take_segment(&cursor, sg_dma_len(sg));
        if (!bytes)
            continue;
        out = &req->sgl[used++];
        out->dma_addr = sg_dma_address(sg);
        out->len = bytes;
        /* Peer resources need not have struct pages. Premapped MM uses only
         * dma_addr/len; pg and offset intentionally remain zero. */
        if (used > 1)
            req->sgl[used - 2].next = out;
    }
    if (cursor.remaining || !used || used >= 65536) {
        kfree(req->sgl);
        req->sgl = NULL;
        return -EINVAL;
    }
    req->sgcnt = used;
    return 0;
}
static long transfer_buffer(struct qdma_file_ctx *ctx, void __user *user)
{
    struct qdma_persistent_transfer io;
    struct persistent_registration *reg;
    struct persistent_request *request = NULL;
    unsigned long qh;
    u64 end;
    long rv;
    bool fence_published = false;
    if (copy_from_user(&io, user, sizeof(io)))
        return -EFAULT;
    if (!valid_header(io.size, io.version, sizeof(io)) ||
        !reserved_zero(io.reserved, ARRAY_SIZE(io.reserved)) ||
        io.bytes_transferred || io.transfer_duration_ns || io.mapping_generation ||
        !io.length || io.length > U32_MAX ||
        check_add_overflow(io.device_address, io.length, &end))
        return -EINVAL;
    if (atomic_read(&ctx->quarantined))
        return -EIO;
    if (atomic_read(&ctx->uncertain_waits))
        return -EBUSY;
    reg = lookup(ctx, io.handle);
    if (!reg)
        return -ENOENT;
    if (io.length > reg->length) {
        rv = -EINVAL;
        goto put_reg;
    }
    rv = queue_ready(ctx->xcdev, reg->direction, &qh);
    if (rv)
        goto put_reg;
    request = kzalloc(sizeof(*request), GFP_KERNEL);
    if (!request) {
        rv = -ENOMEM;
        goto put_reg;
    }
    request->reg = reg;
    refcount_set(&request->refs, 1);
    init_completion(&request->done);
    INIT_LIST_HEAD(&request->quarantine_node);
    INIT_WORK(&request->complete_work, complete_work);
    spin_lock_init(&request->state_lock);
    rv = dma_resv_lock_interruptible(reg->buf->resv, NULL);
    if (rv)
        goto free_request;
    if (atomic_read(&reg->busy)) {
        rv = -EBUSY;
        goto unlock;
    }
    /* C2H writes GPU memory, so wait all previous readers and writers; H2C
     * reads it, so wait previous writers. Both include KERNEL migration. */
    rv = wait_dependencies(reg, reg->direction ? DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
    if (rv)
        goto unlock;
    rv = map_locked(reg);
    if (rv)
        goto unlock;
    rv = exact_sgl(reg, &request->req, (u32)io.length);
    if (rv)
        goto unlock;
    request->fence = new_fence(reg);
    if (!request->fence) {
        rv = -ENOMEM;
        goto unlock;
    }
    rv = dma_resv_reserve_fences(reg->buf->resv, 1);
    if (rv)
        goto unlock;
    request->req.count = io.length;
    request->req.ep_addr = io.device_address;
    request->req.write = reg->direction;
    request->req.dma_mapped = 1;
    request->req.h2c_eot = 1;
    request->req.timeout_ms = 0; /* own wait never removes native request */
    request->req.fp_done = request_done;
    io.mapping_generation = reg->generation;
    atomic_set(&reg->busy, 1);
    atomic64_inc(&ctx->active);
    atomic64_inc(&global_active);
    dma_resv_add_fence(reg->buf->resv, &request->fence->base,
                       reg->direction ? DMA_RESV_USAGE_READ : DMA_RESV_USAGE_WRITE);
    fence_published = true;
    refcount_inc(&request->refs); /* hardware owns accepted request */
    request->start_ns = ktime_get_ns();
    rv = ctx->xcdev->fp_rw(ctx->xcdev->xcb->xpdev->dev_hndl, qh, &request->req);
    /* In premapped MM mode qdma_request_submit returns <0 only before
     * qdma_work_queue_add. After enqueue it returns zero even if descriptor
     * processing fails. Therefore only negative return proves not submitted. */
    if (rv < 0) {
        atomic_set(&reg->busy, 0);
        atomic64_dec(&ctx->active);
        atomic64_dec(&global_active);
        dma_fence_set_error(&request->fence->base, rv);
        dma_fence_signal(&request->fence->base);
        refcount_dec(&request->refs);
    }
    dma_resv_unlock(reg->buf->resv);
    if (rv < 0)
        goto put_request;
    rv = wait_for_completion_interruptible_timeout(&request->done, msecs_to_jiffies(10000));
    if (rv <= 0) {
        unsigned long flags;
        spin_lock_irqsave(&request->state_lock, flags);
        if (!request->finished) {
            request->uncertain = true;
            atomic_inc(&ctx->uncertain_waits);
        }
        spin_unlock_irqrestore(&request->state_lock, flags);
        rv = rv ? rv : -ETIMEDOUT;
        pr_warn("wait ended %ld for handle %llu; native request and mapping retained until hardware completion\n",
                 rv, reg->handle);
        goto put_request;
    }
    rv = request->error;
    if (!rv) {
        flush_work(&request->complete_work);
        io.bytes_transferred = request->bytes;
        io.transfer_duration_ns = request->duration_ns;
        if (copy_to_user(user, &io, sizeof(io)))
            rv = -EFAULT;
    }
put_request:
    request_put(request);
    return rv;
unlock:
    dma_resv_unlock(reg->buf->resv);
free_request:
    WARN_ON(fence_published);
    if (request->fence)
        dma_fence_put(&request->fence->base);
    kfree(request->req.sgl);
    kfree(request);
put_reg:
    registration_put(reg);
    return rv;
}
static long unregister_buffer(struct qdma_file_ctx *ctx, void __user *user)
{
    struct qdma_persistent_unregister io;
    struct persistent_registration *reg, *found = NULL;
    int rv = -ENOENT;
    if (copy_from_user(&io, user, sizeof(io)))
        return -EFAULT;
    if (!valid_header(io.size, io.version, sizeof(io)) ||
        !reserved_zero(io.reserved, ARRAY_SIZE(io.reserved)))
        return -EINVAL;
    mutex_lock(&ctx->lock);
    list_for_each_entry(reg, &ctx->registrations, node) {
        if (reg->handle != io.handle)
            continue;
        /* A lookup may already own a ref before setting busy. Refcount==1
         * proves there are no in-flight transfer/stat/lookups at this point. */
        if (atomic_read(&reg->busy) || kref_read(&reg->refs) != 1) {
            rv = -EBUSY;
            break;
        }
        list_del_init(&reg->node);
        found = reg;
        atomic64_inc(&ctx->unregistered);
        rv = 0;
        break;
    }
    mutex_unlock(&ctx->lock);
    if (found)
        registration_put(found);
    return rv;
}
static long read_stats(struct qdma_file_ctx *ctx, void __user *user)
{
    struct qdma_persistent_stats io;
    struct persistent_registration *reg = NULL;
    if (copy_from_user(&io, user, sizeof(io)))
        return -EFAULT;
    if (!valid_header(io.size, io.version, sizeof(io)) ||
        !reserved_zero(io.reserved, ARRAY_SIZE(io.reserved)))
        return -EINVAL;
    if (io.handle) {
        reg = lookup(ctx, io.handle);
        if (!reg)
            return -ENOENT;
        io.registrations = 1;
        io.unregistrations = 0;
        io.live_registrations = 1;
        io.mappings = atomic64_read(&reg->mappings);
        io.invalidations = atomic64_read(&reg->invalidations);
        io.remaps = atomic64_read(&reg->remaps);
        io.active_requests = atomic_read(&reg->busy);
        io.completed_requests = atomic64_read(&reg->completed);
        io.quarantined_requests = atomic64_read(&reg->quarantine);
        io.mapping_generation = READ_ONCE(reg->generation);
        registration_put(reg);
    } else {
        io.registrations = atomic64_read(&ctx->registered);
        io.unregistrations = atomic64_read(&ctx->unregistered);
        io.live_registrations = atomic64_read(&ctx->live);
        io.mappings = atomic64_read(&ctx->mappings);
        io.invalidations = atomic64_read(&ctx->invalidations);
        io.remaps = atomic64_read(&ctx->remaps);
        io.active_requests = atomic64_read(&ctx->active);
        io.completed_requests = atomic64_read(&ctx->completed);
        io.quarantined_requests = atomic64_read(&ctx->quarantine);
        io.mapping_generation = 0;
    }
    return copy_to_user(user, &io, sizeof(io)) ? -EFAULT : 0;
}
long qdma_persistent_ioctl(struct qdma_file_ctx *ctx, unsigned int cmd, unsigned long arg)
{
    void __user *user = (void __user *)arg;
    if (cmd == QDMA_CDEV_IOCTL_CAPS) {
        struct qdma_persistent_caps io;
        if (copy_from_user(&io, user, sizeof(io)))
            return -EFAULT;
        if (!valid_header(io.size, io.version, sizeof(io)) ||
            !reserved_zero(io.reserved, ARRAY_SIZE(io.reserved)))
            return -EINVAL;
        memset(&io, 0, sizeof(io));
        io.size = sizeof(io);
        io.version = QDMA_PERSISTENT_ABI_VERSION;
        io.features = QDMA_PERSISTENT_FEATURES;
        io.max_transfer_bytes = U32_MAX;
        strscpy(io.build_id, QDMA_PERSISTENT_BUILD_ID, sizeof(io.build_id));
        return copy_to_user(user, &io, sizeof(io)) ? -EFAULT : 0;
    }
    switch (cmd) {
    case QDMA_CDEV_IOCTL_REGISTER: return register_buffer(ctx, user);
    case QDMA_CDEV_IOCTL_TRANSFER_REGISTERED: return transfer_buffer(ctx, user);
    case QDMA_CDEV_IOCTL_UNREGISTER: return unregister_buffer(ctx, user);
    case QDMA_CDEV_IOCTL_STATS: return read_stats(ctx, user);
    default: return -ENOTTY;
    }
}
