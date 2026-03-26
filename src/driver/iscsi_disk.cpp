#include "ublkpp/drivers/iscsi_disk.hpp"

#include <sisl/logging/logging.h>
#include <sisl/options/options.h>
#include <sisl/utility/thread_factory.hpp>
#include <ublksrv.h>

extern "C" {
#include <iscsi/iscsi.h>
#include <iscsi/scsi-lowlevel.h>
#include <poll.h>
#include <sys/eventfd.h>
}

#include "lib/logging.hpp"

SISL_LOGGING_DEF(libiscsi)

namespace ublkpp {

/// TODO Should be discoverable from Inquiry Pages
constexpr auto k_physical_block_size = 4 * Ki;

struct iscsi_session {
    ~iscsi_session() {
        if (url) iscsi_destroy_url(url);
        if (0 <= evfd) {
            // Write a very large value to signal to the event loop that we wish to shutdown
            uint64_t data = UINT32_MAX;
            if (auto err = write(evfd, &data, sizeof(uint64_t)); 0 > err) {
                DLOGE("Failed to write to eventfd! {}", strerror(errno))
            }
        }
        if (ev_loop.joinable()) ev_loop.join();
    }

    std::thread ev_loop;
    int evfd{-1};
    iscsi_context* ctx{nullptr};
    iscsi_url* url{nullptr};
};

static void iscsi_log(int level, const char* message) {
    if (1 >= level) {
        LOGERRORMOD(libiscsi, "{}", message);
    } else if (2 == level) {
        LOGINFOMOD(libiscsi, "{}", message);
    } else if (3 == level) {
        LOGDEBUGMOD(libiscsi, "{}", message);
    } else {
        LOGTRACEMOD(libiscsi, "{}", message);
    }
}

static std::unique_ptr< iscsi_session > create_iscsi_session(std::string const& url) {
    auto session = std::make_unique< iscsi_session >();
    DEBUG_ASSERT(session, "Failed to allocate iSCSI session!");

    if (session->ctx = iscsi_create_context("iqn.2002-10.com.ublkpp:client"); !session->ctx) {
        DLOGE("failed to init context")
        return nullptr;
    }
    iscsi_set_log_level(session->ctx, (spdlog::level::level_enum::critical - module_level_ublk_drivers) * 2);
    iscsi_set_log_fn(session->ctx, iscsi_log);

    if (iscsi_set_alias(session->ctx, "ublkpp")) return nullptr;

    // Attempt to parse the URL
    session->url = iscsi_parse_full_url(session->ctx, url.data());
    if (!session->url) {
        DLOGE("{}", iscsi_get_error(session->ctx))
        return nullptr;
    }

    return session;
}

static bool iscsi_login(std::unique_ptr< iscsi_session >& session) {
    iscsi_set_session_type(session->ctx, ISCSI_SESSION_NORMAL);
    iscsi_set_header_digest(session->ctx, ISCSI_HEADER_DIGEST_NONE);
    iscsi_set_targetname(session->ctx, session->url->target);

    if (0 != iscsi_full_connect_sync(session->ctx, session->url->portal, session->url->lun)) {
        DLOGE("{}", iscsi_get_error(session->ctx))
        return false;
    }
    return (0 != iscsi_is_logged_in(session->ctx));
}

static std::pair< uint64_t, uint32_t > probe_topology(std::unique_ptr< iscsi_session >& session) {
    auto task = iscsi_readcapacity16_sync(session->ctx, session->url->lun);
    if (!task || task->status != SCSI_STATUS_GOOD) {
        DLOGE("Failed to send readcapacity16 command");
        if (task) { scsi_free_scsi_task(task); }
        return std::make_pair(0UL, 0U);
    }

    uint64_t capacity{0UL};
    uint32_t block_size{0U};
    if (auto rc16 = static_cast< scsi_readcapacity16* >(scsi_datain_unmarshall(task)); rc16) {
        capacity = rc16->block_length * (rc16->returned_lba + 1);
        block_size = rc16->block_length;
        DLOGD("Logged into LUN with [sz:{}|bs:{}]", capacity, block_size)
    } else
        DLOGE("Failed to unmarshall ReadCapacity16 data")
    scsi_free_scsi_task(task);
    return std::make_pair(capacity, block_size);
}

iSCSIDisk::iSCSIDisk(std::string const& url) {
    direct_io = true;
    uses_ublk_iouring = false;

    // Establish iSCSI login
    if (_session = create_iscsi_session(url); !_session)
        throw std::runtime_error(fmt::format("Failed to attach iSCSI target: {}", url));
    if (!iscsi_login(_session)) throw std::runtime_error("Could not login to target");

    auto const [capacity, lba_size] = probe_topology(_session);
    if (0 == capacity) throw std::runtime_error("Could not probe LUN to discover capacity");
    auto const block_shift = ilog2(lba_size);

    auto& our_params = *params();
    our_params.basic.logical_bs_shift = static_cast< uint8_t >(block_shift);
    our_params.basic.physical_bs_shift = static_cast< uint8_t >(block_shift);
    our_params.basic.dev_sectors = capacity >> SECTOR_SHIFT;

    // TODO Implement discard
    our_params.types |= ~UBLK_PARAM_TYPE_DISCARD;

    // Start an eventfd file so our target thread can wake up this iscsi service thread
    _session->evfd = eventfd(0, 0);
    if (0 > _session->evfd) throw std::runtime_error(fmt::format("Could not initialize eventfd: {}", strerror(errno)));

    DLOGD("iSCSI device [{}:{}:{}]!", _session->url->target, lba_size, k_physical_block_size)
}

iSCSIDisk::~iSCSIDisk() = default;

std::string iSCSIDisk::id() const { return _session->url->target; }

// Initialize our event loop before we start getting I/O
std::list< int > iSCSIDisk::open_for_uring(int const) {
    using namespace std::chrono_literals;
    _session->ev_loop = sisl::named_thread("iscsi_evloop", [ctx = _session->ctx, evfd = _session->evfd] {
        pollfd ev_pfd[2] = {{.fd = evfd, .events = POLLIN, .revents = 0},
                            {.fd = iscsi_get_fd(ctx), .events = 0, .revents = 0}};
        auto stopping = false;
        while (!stopping) {
            ev_pfd[1].fd = iscsi_get_fd(ctx);
            ev_pfd[1].events = iscsi_which_events(ctx);
            if (0 == ev_pfd[1].events) {
                std::this_thread::sleep_for(100ms);
                continue;
            }
            if (auto res = poll(ev_pfd, 2, -1); 0 > res) {
                if (EINTR == errno) continue;
                DLOGE("Poll failed: {}", strerror(errno))
                stopping = true;
            }
            if (ev_pfd[0].revents & POLLIN) {
                uint64_t data;
                if (auto ret = read(evfd, &data, sizeof(uint64_t)); sizeof(uint64_t) != ret) {
                    DLOGE("Could not read from eventfd: {}", strerror(errno));
                }
                // We will not have more than 4GiB of events, so this means shutdown
                if (UINT32_MAX <= data) stopping = true;
            }
            if (stopping) iscsi_logout_sync(ctx);
            if ((ev_pfd[1].revents & (POLLIN | POLLOUT)) || stopping) {
                if (iscsi_service(ctx, ev_pfd[1].revents) < 0) { DLOGE("iSCSI failed: {}", iscsi_get_error(ctx)) }
            }
        }
        iscsi_destroy_context(ctx);
        close(evfd);
    });
    return {};
}

void iSCSIDisk::collect_async(ublksrv_queue const*, std::list< async_result >& completed) {
    auto lck = std::scoped_lock< std::mutex >(pending_results_lck);
    completed.splice(completed.end(), std::move(pending_results));
}

void iSCSIDisk::async_complete(ublksrv_queue const* q, async_result&& result) {
    {
        auto lck = std::scoped_lock< std::mutex >(pending_results_lck);
        pending_results.emplace_back(std::move(result));
    }
    ublksrv_queue_send_event(q);
}

io_result iSCSIDisk::handle_flush(ublksrv_queue const*, ublk_io_data const* ublk_io, sub_cmd_t sub_cmd) {
    DLOGT("Flush : [tag:{:#0x}] ublk io [sub_cmd:{}]", ublk_io->tag, ublkpp::to_string(sub_cmd))
    if (direct_io) return 0;
    return std::unexpected(std::make_error_condition(std::errc::not_supported));
}

io_result iSCSIDisk::handle_discard(ublksrv_queue const*, ublk_io_data const* ublk_io, sub_cmd_t sub_cmd, uint32_t len,
                                    uint64_t addr) {
    auto const lba = addr >> params()->basic.logical_bs_shift;
    DLOGD("DISCARD : [tag:{:#0x}] ublk io [lba:{:#0x}|len:{:#0x}|sub_cmd:{}]", ublk_io->tag, lba, len,
          ublkpp::to_string(sub_cmd))
    return std::unexpected(std::make_error_condition(std::errc::not_supported));
}

struct iscsi_cb_data {
    ublk_io_data const* io;
    int tag;
    sub_cmd_t sub_cmd;
    std::shared_ptr< iSCSIDisk > device;
    ublksrv_queue const* queue;
    int len;
    scsi_iovec io_vec[16]{{0, 0}};
};

void iSCSIDisk::__iscsi_rw_cb(iscsi_context* ctx, int status, void* data, void* private_data) {
    auto cb_data = reinterpret_cast< iscsi_cb_data* >(private_data);
    int result = cb_data->len;
    if (SCSI_STATUS_GOOD != status) [[unlikely]] {
        result = -EIO;
        auto task = reinterpret_cast< scsi_task* >(data);
        DLOGW("iSCSI cmd returned error: [tag:{:#0x}], [status:{}|key:{:#0x}|ascq:{:#0x}] iscsi_err: {}", cb_data->tag,
              status, (uint8_t)task->sense.key, task->sense.ascq, iscsi_get_error(ctx));
        if (SCSI_SENSE_ILLEGAL_REQUEST == task->sense.key) {
            // The LUN is offline but the target still exists, drive reset?
            if (SCSI_SENSE_ASCQ_LOGICAL_UNIT_NOT_SUPPORTED == task->sense.ascq) { result = -EAGAIN; }
        }
    } else
        DLOGT("Got iSCSI completion: [tag:{:#0x}], status: {}", cb_data->tag, status);
    cb_data->device->async_complete(cb_data->queue, async_result{cb_data->io, cb_data->sub_cmd, result});
    scsi_free_scsi_task(reinterpret_cast< scsi_task* >(data));
    delete cb_data;
}

io_result iSCSIDisk::async_iov(ublksrv_queue const* q, ublk_io_data const* ublk_io, sub_cmd_t sub_cmd, iovec* iovecs,
                               uint32_t nr_vecs, uint64_t addr) {
    auto const op = ublksrv_get_op(ublk_io->iod);
    int const len = __iovec_len(iovecs, iovecs + nr_vecs);

    // Convert the absolute address to an LBA offset
    auto const lba = addr >> params()->basic.logical_bs_shift;

    DLOGT("{} : [tag:{:#0x}] ublk io [lba:{:#0x}|len:{:#0x}|sub_cmd:{}]", op == UBLK_IO_OP_READ ? "READ" : "WRITE",
          ublk_io->tag, lba, len, ublkpp::to_string(sub_cmd))

    // We copy the iovec here since libiscsi does not make it stable
    auto cb_data = new iscsi_cb_data(ublk_io, ublk_io->tag, sub_cmd,
                                     dynamic_pointer_cast< iSCSIDisk >(shared_from_this()), q, len);
    if (!cb_data) return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
    for (auto i = 0U; nr_vecs > i; ++i) {
        cb_data->io_vec[i].iov_base = iovecs[i].iov_base;
        cb_data->io_vec[i].iov_len = iovecs[i].iov_len;
    }

    auto task = (UBLK_IO_OP_READ == op)
        ? iscsi_read16_iov_task(_session->ctx, _session->url->lun, lba, len, block_size(), 0, 0, 0, 0, 0, __iscsi_rw_cb,
                                cb_data, cb_data->io_vec, nr_vecs)
        : iscsi_write16_iov_task(_session->ctx, _session->url->lun, lba, NULL, len, block_size(), 0, 0, 0, 0, 0,
                                 __iscsi_rw_cb, cb_data, cb_data->io_vec, nr_vecs);

    if (!task) {
        DLOGE("Failed {} to iSCSI LUN. {}", op == UBLK_IO_OP_READ ? "READ" : "WRITE", iscsi_get_error(_session->ctx));
        return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
    }

    uint64_t data = 1;
    if (auto ret = write(_session->evfd, &data, sizeof(uint64_t)); sizeof(uint64_t) != ret) {
        DLOGE("Could not write to eventfd: {}", strerror(errno));
        return std::unexpected(std::make_error_condition(std::errc::io_error));
    }
    return 1;
}

io_result iSCSIDisk::sync_iov(uint8_t op, iovec* iovecs, uint32_t nr_vecs, off_t addr) noexcept {
    auto const len = __iovec_len(iovecs, iovecs + nr_vecs);

    // Convert the absolute address to an LBA offset
    auto const lba = addr >> params()->basic.logical_bs_shift;

    DLOGT("{} : [INTERNAL] ublk io [lba:{:#0x}|len:{:#0x}]", op == UBLK_IO_OP_READ ? "READ" : "WRITE", lba, len)

    auto task = (UBLK_IO_OP_READ == op)
        ? iscsi_read16_iov_sync(_session->ctx, _session->url->lun, lba, len, block_size(), 0, 0, 0, 0, 0,
                                reinterpret_cast< scsi_iovec* >(iovecs), nr_vecs)
        : iscsi_write16_iov_sync(_session->ctx, _session->url->lun, lba, NULL, len, block_size(), 0, 0, 0, 0, 0,
                                 reinterpret_cast< scsi_iovec* >(iovecs), nr_vecs);
    if (!task) return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
    io_result res = len;
    if (SCSI_STATUS_GOOD != task->status) {
        DLOGW("iSCSI cmd returned error: [status:{}] iscsi_err: ", task->status, iscsi_get_error(_session->ctx));
        if (SCSI_SENSE_ILLEGAL_REQUEST == task->sense.key) {
            // The LUN is offline but the target still exists, drive reset?
            if (SCSI_SENSE_ASCQ_LOGICAL_UNIT_NOT_SUPPORTED == task->sense.ascq) {
                res = std::unexpected(std::make_error_condition(std::errc::resource_unavailable_try_again));
            }
        }
        res = std::unexpected(std::make_error_condition(std::errc::io_error));
    }
    scsi_free_scsi_task(task);
    return res;
}
} // namespace ublkpp
