#include "hakios-metal/gpu.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace hakios::metal {

namespace {
constexpr uint64_t kPage = 0x4000;  // Darwin's page; the driver rounds to its own (4 KB) inside it

MsmTimespec deadline(Transport& t, int64_t ns) {
    // the driver takes a deadline, not a length: a time of the clock that goes on while the machine sleeps
    // (mach_continuous_time, which is Darwin's CLOCK_MONOTONIC), as Linux's msm takes one of CLOCK_MONOTONIC
    if (ns < 0) {
        ns = 0;
    }
    const int64_t now = t.nowNs();
    const int64_t at = (ns > std::numeric_limits<int64_t>::max() - now)
                           ? std::numeric_limits<int64_t>::max()
                           : (now + ns);
    return MsmTimespec{at / 1000000000, at % 1000000000};
}
}  // namespace

Gpu::Gpu(std::unique_ptr<Transport> transport)
    : poolEnabled_(transport != nullptr && transport->canPool()),
      t_(std::move(transport)) {
    if (const char* envMb = std::getenv("HAKIOS_BO_POOL_MAX_MB"); envMb != nullptr && *envMb != '\0') {
        char* end = nullptr;
        errno = 0;
        const unsigned long long mb = std::strtoull(envMb, &end, 10);
        if (errno == 0 && end != envMb && *end == '\0') {
            if (mb == 0) {
                maxPoolTotalBytes_ = 0;
                maxPoolBoBytes_ = 0;
                poolEnabled_ = false;
            } else {
                const uint64_t maxMb = kMaxAllowedPoolTotalBytes >> 20;
                const uint64_t cappedMb = (mb > maxMb) ? maxMb : uint64_t(mb);
                maxPoolTotalBytes_ = cappedMb << 20;
                maxPoolBoBytes_ = std::min<uint64_t>(kMaxPoolBoBytes, maxPoolTotalBytes_);
            }
        }
    }
    if (const char* envEntries = std::getenv("HAKIOS_BO_POOL_MAX_ENTRIES"); envEntries != nullptr && *envEntries != '\0') {
        char* end = nullptr;
        errno = 0;
        const unsigned long long cnt = std::strtoull(envEntries, &end, 10);
        if (errno == 0 && end != envEntries && *end == '\0') {
            if (cnt == 0) {
                maxPoolEntries_ = 0;
                poolEnabled_ = false;
            } else {
                maxPoolEntries_ = (cnt > kMaxAllowedPoolEntries) ? kMaxAllowedPoolEntries : size_t(cnt);
            }
        }
    }
}

int Gpu::param(uint32_t which, uint64_t* value) {
    MsmParam p{};
    p.pipe = kPipe3D;
    p.param = which;
    int e = t_->call(kGetParam, &p, sizeof(p));
    if (e == 0) {
        *value = p.value;
    }
    return e;
}

uint32_t Gpu::chipId() {
    uint64_t v = 0;
    return param(kParamChipId, &v) == 0 ? uint32_t(v) : 0;
}

uint64_t Gpu::faults() {
    uint64_t v = 0;
    return param(kParamFaults, &v) == 0 ? v : 0;
}

Gpu::~Gpu() { drainPool(); }

void Gpu::setPoolEnabled(bool enabled) {
    bool drain = false;
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        poolEnabled_ = enabled && t_ != nullptr && maxPoolTotalBytes_ > 0 && maxPoolEntries_ > 0;
        drain = !poolEnabled_;
    }
    if (drain) {
        drainPool();
    }
}

uint64_t Gpu::pooledBytes() const {
    std::lock_guard<std::mutex> lk(poolMutex_);
    return pooledBytes_;
}

size_t Gpu::pooledCount() const {
    std::lock_guard<std::mutex> lk(poolMutex_);
    return pool_.size();
}

void Gpu::addAllocated(uint64_t bytes) {
    allocated_.fetch_add(bytes, std::memory_order_relaxed);
}

void Gpu::subAllocated(uint64_t bytes) {
    uint64_t cur = allocated_.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t next = (cur >= bytes) ? (cur - bytes) : 0;
        if (allocated_.compare_exchange_weak(cur, next, std::memory_order_relaxed)) {
            break;
        }
    }
}

void Gpu::destroyPooledBo(const PooledBo& e) {
    if (t_ == nullptr) {
        return;
    }
    if (e.cpu != nullptr) {
        t_->unmap(e.handle, e.cpu);
    }
    DrmGemClose c{e.handle, 0};
    t_->call(kGemClose, &c, sizeof(c));
}

void Gpu::drainPool() {
    std::vector<PooledBo> dead;
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        dead.swap(pool_);
        pooledBytes_ = 0;
    }
    for (const PooledBo& e : dead) {
        destroyPooledBo(e);
    }
}

void Gpu::noteSubmitted(const Bo& bo, uint32_t queue, uint32_t fence) {
    if (fence == 0) {
        return;
    }
    const uint32_t normQ = (queue >= Bo::kNoQueue) ? Bo::kMultiQueue : queue;
    uint32_t curQ = bo.lastQueue_.load(std::memory_order_relaxed);
    while (curQ != normQ && curQ != Bo::kMultiQueue) {
        const uint32_t nextQ = (curQ == Bo::kNoQueue) ? normQ : Bo::kMultiQueue;
        if (bo.lastQueue_.compare_exchange_weak(curQ, nextQ, std::memory_order_relaxed)) {
            break;
        }
    }
    uint32_t curF = bo.lastFence_.load(std::memory_order_relaxed);
    while ((curF == 0 || int32_t(fence - curF) > 0) &&
           !bo.lastFence_.compare_exchange_weak(curF, fence, std::memory_order_relaxed)) {
    }
}

void Gpu::advanceCompletedFence(uint32_t queue, uint32_t fence) {
    if (fence == 0 || queue >= Bo::kNoQueue) {
        return;
    }
    std::lock_guard<std::mutex> lk(poolMutex_);
    for (QueueFence& qf : completedFences_) {
        if (qf.queue == queue) {
            if (qf.fence == 0 || int32_t(fence - qf.fence) > 0) {
                qf.fence = fence;
            }
            return;
        }
    }
    if (completedFences_.size() >= kMaxTrackedQueues) {
        completedFences_.erase(completedFences_.begin());
    }
    completedFences_.push_back(QueueFence{queue, fence});
}

bool Gpu::isPooledBoIdleLocked(const PooledBo& e) const {
    if (e.lastFence == 0) {
        return true;
    }
    if (e.lastQueue >= Bo::kNoQueue) {
        return false;
    }
    for (const QueueFence& qf : completedFences_) {
        if (qf.queue == e.lastQueue) {
            return qf.fence != 0 && int32_t(qf.fence - e.lastFence) >= 0;
        }
    }
    return false;
}

void Gpu::evictIdleLocked(size_t extraEntries, uint64_t extraBytes, std::vector<PooledBo>& evicted) {
    size_t idx = 0;
    while (idx < pool_.size() &&
           (pool_.size() + extraEntries > maxPoolEntries_ ||
            pooledBytes_ > maxPoolTotalBytes_ - extraBytes)) {
        if (isPooledBoIdleLocked(pool_[idx])) {
            evicted.push_back(pool_[idx]);
            pooledBytes_ = (pooledBytes_ >= pool_[idx].size) ? (pooledBytes_ - pool_[idx].size) : 0;
            pool_.erase(pool_.begin() + std::ptrdiff_t(idx));
        } else {
            ++idx;
        }
    }
}

void Gpu::reinsertBusyLocked(std::vector<PooledBo>& busy, std::vector<PooledBo>& evicted) {
    if (busy.empty()) {
        return;
    }
    size_t insertPos = 0;
    for (const PooledBo& b : busy) {
        if (!poolEnabled_ || maxPoolEntries_ == 0 || b.size > maxPoolTotalBytes_) {
            evicted.push_back(b);
            continue;
        }
        evictIdleLocked(1, b.size, evicted);
        if (pool_.size() < maxPoolEntries_ && pooledBytes_ <= maxPoolTotalBytes_ - b.size) {
            pool_.insert(pool_.begin() + std::ptrdiff_t(insertPos), b);
            pooledBytes_ += b.size;
            ++insertPos;
        } else {
            evicted.push_back(b);
        }
    }
    busy.clear();
}

void Gpu::reclaimIdlePool() {
    std::vector<PooledBo> evicted;
    std::vector<PooledBo> toProbe;
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        for (const PooledBo& entry : pool_) {
            if (isPooledBoIdleLocked(entry)) {
                evicted.push_back(entry);
            } else {
                toProbe.push_back(entry);
            }
        }
        pool_.clear();
        pooledBytes_ = 0;
    }
    std::vector<PooledBo> stillBusy;
    for (const PooledBo& p : toProbe) {
        MsmGemCpuPrep prep{p.handle, kPrepWrite | kPrepNoSync, MsmTimespec{0, 0}};
        if (t_ != nullptr && t_->call(kGemCpuPrep, &prep, sizeof(prep)) == 0) {
            if (p.lastQueue < Bo::kNoQueue && p.lastFence != 0) {
                advanceCompletedFence(p.lastQueue, p.lastFence);
            }
            evicted.push_back(p);
        } else {
            stillBusy.push_back(p);
        }
    }
    if (!stillBusy.empty()) {
        std::lock_guard<std::mutex> lk(poolMutex_);
        reinsertBusyLocked(stillBusy, evicted);
    }
    for (const PooledBo& e : evicted) {
        destroyPooledBo(e);
    }
}

std::unique_ptr<Bo> Gpu::newBo(uint64_t size, uint32_t flags, int* error) {
    int ignoredError = 0;
    if (error == nullptr) {
        error = &ignoredError;
    }
    *error = 0;
    if (t_ == nullptr) {
        *error = ENODEV;
        return nullptr;
    }
    if (size == 0) {
        *error = EINVAL;
        return nullptr;
    }
    if (size > std::numeric_limits<uint64_t>::max() - (kPage - 1)) {
        *error = ENOMEM;
        return nullptr;
    }
    const uint64_t nSize = (size + kPage - 1) & ~(kPage - 1);
    const uint32_t matchFlags = flags & ~kBoNoZero;

    if (nSize <= maxPoolBoBytes_) {
        PooledBo hit{};
        bool found = false;
        PooledBo probe{};
        bool haveProbe = false;
        {
            std::lock_guard<std::mutex> lk(poolMutex_);
            if (poolEnabled_ && !pool_.empty()) {
                size_t bestIdleIdx = pool_.size();
                uint64_t bestIdleSize = std::numeric_limits<uint64_t>::max();
                size_t bestBusyIdx = pool_.size();
                uint64_t bestBusySize = std::numeric_limits<uint64_t>::max();

                // Match pooled BOs in [nSize, 2 * nSize] (cap oversize waste ratio to <= 2x):
                // - Prefer already fence-completed BOs: smallest matching size, newest first (LIFO).
                // - Otherwise pick one unconfirmed BO to probe outside poolMutex_: smallest size, oldest first.
                for (size_t idx = pool_.size(); idx-- > 0;) {
                    const PooledBo& cand = pool_[idx];
                    if (cand.flags != matchFlags || cand.size < nSize || cand.size - nSize > nSize) {
                        continue;
                    }
                    if (isPooledBoIdleLocked(cand)) {
                        if (cand.size < bestIdleSize) {
                            bestIdleSize = cand.size;
                            bestIdleIdx = idx;
                            if (bestIdleSize == nSize) {
                                break;
                            }
                        }
                    } else if (cand.size <= bestBusySize) {
                        bestBusySize = cand.size;
                        bestBusyIdx = idx;
                    }
                }

                if (bestIdleIdx < pool_.size()) {
                    hit = pool_[bestIdleIdx];
                    pooledBytes_ = (pooledBytes_ >= hit.size) ? (pooledBytes_ - hit.size) : 0;
                    pool_.erase(pool_.begin() + std::ptrdiff_t(bestIdleIdx));
                    found = true;
                } else if (bestBusyIdx < pool_.size()) {
                    probe = pool_[bestBusyIdx];
                    pooledBytes_ = (pooledBytes_ >= probe.size) ? (pooledBytes_ - probe.size) : 0;
                    pool_.erase(pool_.begin() + std::ptrdiff_t(bestBusyIdx));
                    haveProbe = true;
                }
            }
        }
        if (haveProbe) {
            MsmGemCpuPrep prep{probe.handle, kPrepWrite | kPrepNoSync, MsmTimespec{0, 0}};
            if (t_->call(kGemCpuPrep, &prep, sizeof(prep)) == 0) {
                if (probe.lastQueue < Bo::kNoQueue && probe.lastFence != 0) {
                    advanceCompletedFence(probe.lastQueue, probe.lastFence);
                }
                hit = probe;
                found = true;
            } else {
                std::vector<PooledBo> evicted;
                std::vector<PooledBo> busy{probe};
                {
                    std::lock_guard<std::mutex> lk(poolMutex_);
                    reinsertBusyLocked(busy, evicted);
                }
                for (const PooledBo& e : evicted) {
                    destroyPooledBo(e);
                }
            }
        }
        if (found) {
            if (!(flags & kBoNoZero) && hit.cpu != nullptr) {
                std::memset(hit.cpu, 0, size_t(hit.size));
            }
            std::unique_ptr<Bo> bo(new Bo(*this, hit.handle, hit.size, hit.iova, hit.cpu));
            bo->flags_ = matchFlags;
            bo->counted_ = true;
            addAllocated(hit.size);
            return bo;
        }
    }

    MsmGemNew n{};
    n.size = nSize;
    n.flags = flags;
    int err = t_->call(kGemNew, &n, sizeof(n));
    if (err == ENOMEM) {
        reclaimIdlePool();
        n = MsmGemNew{};
        n.size = nSize;
        n.flags = flags;
        err = t_->call(kGemNew, &n, sizeof(n));
    }
    if ((*error = err) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = n.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    void* cpu = t_->map(n.handle, n.size);
    if (cpu == nullptr) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        *error = ENOMEM;
        return nullptr;
    }
    std::unique_ptr<Bo> bo(new Bo(*this, n.handle, n.size, info.value, cpu));
    bo->flags_ = matchFlags;
    bo->counted_ = true;
    addAllocated(n.size);
    return bo;
}

std::unique_ptr<Bo> Gpu::surfaceBo(uint32_t surfaceId, SurfaceInfo* out, int* error) {
    int ignoredError = 0;
    if (error == nullptr) {
        error = &ignoredError;
    }
    *error = 0;
    if (t_ == nullptr) {
        *error = ENODEV;
        return nullptr;
    }
    HakiosSurface s{};
    s.surface = surfaceId;
    if ((*error = t_->call(kHakiosSurface, &s, sizeof(s))) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = s.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{s.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    if (out != nullptr) {
        *out = SurfaceInfo{s.size, s.offset, s.rowBytes, s.width, s.height, s.pixelFormat, s.planes};
    }
    return std::unique_ptr<Bo>(new Bo(*this, s.handle, s.size, info.value, t_->map(s.handle, s.size)));
}

void Gpu::disown(std::unique_ptr<Bo> bo) {
    if (bo == nullptr || !bo->owned_) {
        return;
    }
    if (bo->cpu_ != nullptr && t_ != nullptr) {
        t_->unmap(bo->handle_, bo->cpu_);
        bo->cpu_ = nullptr;
    }
    bo->owned_ = false;
    if (bo->counted_) {
        subAllocated(bo->size_);
        bo->counted_ = false;
    }
}

Bo::~Bo() { gpu_.release(*this); }

void Gpu::release(Bo& bo) {
    if (!bo.owned_) {
        return;
    }
    bo.owned_ = false;
    bool recycleCandidate = false;
    if (bo.counted_) {
        subAllocated(bo.size_);
        bo.counted_ = false;
        recycleCandidate = (bo.cpu_ != nullptr && bo.size_ > 0 &&
                            bo.size_ <= maxPoolBoBytes_ && bo.size_ <= maxPoolTotalBytes_);
    }
    if (recycleCandidate && t_ != nullptr) {
        const PooledBo incoming{
            bo.handle_,
            bo.flags_,
            bo.size_,
            bo.iova_,
            bo.cpu_,
            bo.lastQueue_.load(std::memory_order_relaxed),
            bo.lastFence_.load(std::memory_order_relaxed),
        };
        std::vector<PooledBo> evicted;
        std::vector<PooledBo> busyChecked;
        uint64_t busyCheckedBytes = 0;
        bool pooled = false;

        for (;;) {
            PooledBo toProbe{};
            bool needProbe = false;
            {
                std::lock_guard<std::mutex> lk(poolMutex_);
                if (!poolEnabled_ || maxPoolEntries_ == 0 || incoming.size > maxPoolTotalBytes_) {
                    for (const PooledBo& b : busyChecked) {
                        evicted.push_back(b);
                    }
                    busyChecked.clear();
                    busyCheckedBytes = 0;
                    break;
                }

                // If in-flight pooled BOs alone leave no room for incoming, keep those in-flight BOs
                // in the pool and destroy incoming directly instead of evicting an active GPU buffer.
                if (busyChecked.size() >= maxPoolEntries_ ||
                    busyCheckedBytes > maxPoolTotalBytes_ - incoming.size) {
                    reinsertBusyLocked(busyChecked, evicted);
                    busyCheckedBytes = 0;
                    break;
                }

                const size_t wantEntries = busyChecked.size() + 1;
                const uint64_t wantBytes = busyCheckedBytes + incoming.size;
                evictIdleLocked(wantEntries, wantBytes, evicted);

                if (pool_.size() + wantEntries <= maxPoolEntries_ &&
                    pooledBytes_ <= maxPoolTotalBytes_ - wantBytes) {
                    if (!busyChecked.empty()) {
                        pool_.insert(pool_.begin(), busyChecked.begin(), busyChecked.end());
                        pooledBytes_ += busyCheckedBytes;
                        busyChecked.clear();
                        busyCheckedBytes = 0;
                    }
                    pool_.push_back(incoming);
                    pooledBytes_ += incoming.size;
                    pooled = true;
                    break;
                }

                if (pool_.empty()) {
                    reinsertBusyLocked(busyChecked, evicted);
                    busyCheckedBytes = 0;
                    break;
                }

                toProbe = pool_.front();
                pooledBytes_ = (pooledBytes_ >= toProbe.size) ? (pooledBytes_ - toProbe.size) : 0;
                pool_.erase(pool_.begin());
                needProbe = true;
            }

            if (!needProbe) {
                break;
            }

            MsmGemCpuPrep prep{toProbe.handle, kPrepWrite | kPrepNoSync, MsmTimespec{0, 0}};
            if (t_->call(kGemCpuPrep, &prep, sizeof(prep)) == 0) {
                if (toProbe.lastQueue < Bo::kNoQueue && toProbe.lastFence != 0) {
                    advanceCompletedFence(toProbe.lastQueue, toProbe.lastFence);
                }
                evicted.push_back(toProbe);
            } else {
                busyChecked.push_back(toProbe);
                busyCheckedBytes += toProbe.size;
            }
        }

        for (const PooledBo& e : evicted) {
            destroyPooledBo(e);
        }
        if (pooled) {
            return;
        }
    }
    if (t_ != nullptr) {
        if (bo.cpu_ != nullptr) {
            t_->unmap(bo.handle_, bo.cpu_);
        }
        DrmGemClose c{bo.handle_, 0};
        t_->call(kGemClose, &c, sizeof(c));
    }
}

int Gpu::newQueue(uint32_t priority, uint32_t* id) {
    MsmSubmitQueue q{0, priority, 0};
    int e = t_->call(kSubmitQueueNew, &q, sizeof(q));
    if (e == 0) {
        *id = q.id;
    }
    return e;
}

void Gpu::closeQueue(uint32_t id) {
    if (id != 0) {
        {
            std::lock_guard<std::mutex> lk(poolMutex_);
            for (PooledBo& e : pool_) {
                if (e.lastQueue == id && e.lastFence != 0) {
                    e.lastQueue = Bo::kMultiQueue;
                }
            }
            for (auto it = completedFences_.begin(); it != completedFences_.end(); ++it) {
                if (it->queue == id) {
                    completedFences_.erase(it);
                    break;
                }
            }
        }
        uint32_t q = id;
        t_->call(kSubmitQueueClose, &q, sizeof(q));
    }
}

int Gpu::submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
                uint32_t* fence) {
    if (words == 0 || offsetBytes % 4 != 0 || offsetBytes + 4ull * words > commands.size()) {
        return EINVAL;
    }
    // the command buffer is the first of the buffers; the others follow it once each, read or written
    std::vector<MsmSubmitBo> bos;
    bos.reserve(uses.size() + 1);
    bos.push_back(MsmSubmitBo{kSubmitBoRead, commands.handle(), commands.gpuAddress()});
    for (const BoUse& u : uses) {
        if (u.bo == nullptr || u.bo == &commands) {
            continue;
        }
        bool seen = false;
        for (MsmSubmitBo& b : bos) {
            if (b.handle == u.bo->handle()) {
                b.flags |= u.write ? kSubmitBoWrite : kSubmitBoRead;
                seen = true;
            }
        }
        if (!seen) {
            bos.push_back(MsmSubmitBo{u.write ? kSubmitBoWrite : kSubmitBoRead, u.bo->handle(), u.bo->gpuAddress()});
        }
    }
    MsmSubmitCmd cmd{};
    cmd.type = kSubmitCmdBuf;
    cmd.submit_idx = 0;
    cmd.submit_offset = offsetBytes;
    cmd.size = words * 4;
    MsmGemSubmit s{};
    s.flags = kPipe3D;
    s.nr_bos = uint32_t(bos.size());
    s.nr_cmds = 1;
    s.bos = uint64_t(uintptr_t(bos.data()));
    s.cmds = uint64_t(uintptr_t(&cmd));
    s.fence_fd = -1;
    s.queueid = queue;
    int e = t_->call(kGemSubmit, &s, sizeof(s));
    if (e == 0) {
        if (fence != nullptr) {
            *fence = s.fence;
        }
        if (s.fence != 0) {
            noteSubmitted(commands, queue, s.fence);
            for (const BoUse& u : uses) {
                if (u.bo != nullptr && u.bo != &commands) {
                    noteSubmitted(*u.bo, queue, s.fence);
                }
            }
        }
    }
    return e;
}

int Gpu::wait(uint32_t queue, uint32_t fence, int64_t timeoutNs) {
    MsmWaitFence w{};
    w.fence = fence;
    w.timeout = deadline(*t_, timeoutNs);
    w.queueid = queue;
    int e = t_->call(kWaitFence, &w, sizeof(w));
    if (e == 0 && fence != 0) {
        advanceCompletedFence(queue, fence);
    }
    return e;
}

int Gpu::cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs) {
    MsmGemCpuPrep p{bo.handle(), op, deadline(*t_, timeoutNs)};
    int e = t_->call(kGemCpuPrep, &p, sizeof(p));
    if (e == 0 && (op & kPrepWrite) != 0) {
        const uint32_t q = bo.lastQueue_.load(std::memory_order_relaxed);
        const uint32_t f = bo.lastFence_.load(std::memory_order_relaxed);
        if (q < Bo::kNoQueue && f != 0) {
            advanceCompletedFence(q, f);
        }
    }
    return e;
}

void Gpu::cpuFini(const Bo& bo) {
    MsmGemCpuFini f{bo.handle()};
    t_->call(kGemCpuFini, &f, sizeof(f));
}

}  // namespace hakios::metal
