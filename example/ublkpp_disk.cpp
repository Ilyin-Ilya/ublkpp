#include <filesystem>
#include <future>
#include <ostream>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <iomgr/io_environment.hpp>
#include <iomgr/http_server.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include <ublkpp/ublkpp.hpp>
#include <ublkpp/drivers/fs_disk.hpp>
#include <ublkpp/raid/raid0.hpp>
#include <ublkpp/raid/raid1.hpp>

#ifdef HAVE_HOMEBLOCKS
#include <ublkpp/drivers/homeblk_disk.hpp>
#include <homeblks/home_blks.hpp>
#include <homeblks/volume_mgr.hpp>
#endif
#ifdef HAVE_ISCSI
#include <ublkpp/drivers/iscsi_disk.hpp>
#endif

SISL_OPTION_GROUP(
    ublkpp_disk, (uuid, "", "vol_id", "Volume UUID to use (else random)", ::cxxopts::value< std::string >(), ""),
    (loop, "", "loop", "Attach a single device 1-to-1", ::cxxopts::value< std::string >(), "<path>"),
    (raid0, "", "raid0", "Devices for RAID0 device", ::cxxopts::value< std::vector< std::string > >(),
     "<path>[,<path>,...]"),
    (raid1, "", "raid1", "Devices for RAID1 device", ::cxxopts::value< std::vector< std::string > >(),
     "<path>[,<path>,...]"),
    (raid10, "", "raid10", "Devices for RAID10 device", ::cxxopts::value< std::vector< std::string > >(),
     "<path>[,<path>,...]"),
#ifdef HAVE_HOMEBLOCKS
    (capacity, "", "capacity", "HomeBlks disk capacity GiB", ::cxxopts::value< uint32_t >()->default_value("2"),
     "<GiB>"),
    (homeblks_dev, "", "homeblks_dev", "path to the device to run HomeBlocks on", cxxopts::value< std::string >(), ""),
#endif
    (stripe_size, "", "stripe_size", "RAID-0 Stripe Size", ::cxxopts::value< uint32_t >()->default_value("131072"), ""),
    (device_id, "", "device_id", "Recover existing device", cxxopts::value< int32_t >()->default_value("-1"),
     "<ublkid>"))

#ifdef HAVE_HOMEBLOCKS
#define HOMEBLKS_OPTIONS , homeblocks, iomgr
#else
#define HOMEBLKS_OPTIONS
#endif

#define ENABLED_OPTIONS logging, ublkpp_tgt, raid1, fs_disk, ublkpp_disk HOMEBLKS_OPTIONS

SISL_OPTIONS_ENABLE(ENABLED_OPTIONS)

#ifdef HAVE_HOMEBLOCKS
#define HOMEBLKS_MODS , HOMEBLOCKS_LOG_MODS, HOMESTORE_LOG_MODS, IOMGR_LOG_MODS
#else
#define HOMEBLKS_MODS
#endif

#ifdef HAVE_ISCSI
#define ISCSI_MODS , libiscsi
#else
#define ISCSI_MODS
#endif

SISL_LOGGING_INIT(ublksrv, UBLK_LOG_MODS HOMEBLKS_MODS ISCSI_MODS)

///
// Clean shutdown
static std::promise< int > s_stop_code;
static void handle(int signal);
///

using Result = std::expected< std::filesystem::path, std::error_condition >;
static auto k_target = std::unique_ptr< ublkpp::ublkpp_tgt >();

template < typename D >
Result _run_target(boost::uuids::uuid const& vol_id, std::unique_ptr< D >&& dev) {

    // Wait for initialization to complete
    auto res = ublkpp::ublkpp_tgt::run(vol_id, std::move(dev), SISL_OPTIONS["device_id"].as< int32_t >());
    if (!res) { return std::unexpected(res.error()); }
    k_target = std::move(res.value());
    return k_target->device_path();
}

#ifdef HAVE_HOMEBLOCKS
class UblkPPApplication : public homeblocks::HomeBlocksApplication {
public:
    using dev_list_t = std::list< homeblocks::device_info_t >;
    dev_list_t const _devices;
    uint32_t const _io_threads;
    homeblocks::peer_id_t const _id;
    std::shared_ptr< homeblocks::HomeBlocks > _hb;

    explicit UblkPPApplication(dev_list_t const& dev_list, homeblocks::peer_id_t p = boost::uuids::random_generator()(),
                               uint32_t const io_threads = 1) :
            _devices(dev_list), _io_threads(io_threads), _id(p) {}
    ~UblkPPApplication() override = default;

    // implement all the virtual functions in HomeObjectApplication
    uint64_t app_mem_size() const override { return 20; }
    bool spdk_mode() const override { return false; }
    uint32_t threads() const override { return _io_threads; }
    dev_list_t devices() const override { return _devices; }

    std::optional< homeblocks::peer_id_t >
    discover_svc_id(std::optional< homeblocks::peer_id_t > const&) const override {
        return _id;
    }
};
static std::shared_ptr< UblkPPApplication > _app;

static std::shared_ptr< UblkPPApplication > init_homeblocks(std::string const& hb_dev) {
    auto const path = std::filesystem::path(hb_dev);
    if (!std::filesystem::exists(path)) {
        LOGERROR("Homestore device: {} does not exist!", path.native())
        return nullptr;
    }

    auto dev_list = std::list< homeblocks::device_info_t >();
    dev_list.emplace_back(homeblocks::device_info_t{path.native(), homeblocks::DevType::NVME});
    auto app = std::make_shared< UblkPPApplication >(dev_list);
    app->_hb = homeblocks::init_homeblocks(decltype(app)::weak_type(app));

    return app;
}

static auto create_hb_volume(UblkPPApplication& app, boost::uuids::uuid const& vol_uuid) {
    auto known_vols = std::vector< homeblocks::volume_id_t >();
    app._hb->volume_manager()->get_volume_ids(known_vols);
    if (std::ranges::contains(known_vols, vol_uuid)) return std::error_condition();

    homeblocks::VolumeInfo vol_info;
    vol_info.page_size = 4 * Ki;
    vol_info.size_bytes = SISL_OPTIONS["capacity"].as< uint32_t >() * Gi;
    vol_info.id = vol_uuid;
    vol_info.name = "ublkpp_disk";
    LOGINFO("Creating volume: {}", vol_info.to_string());

    return app._hb->volume_manager()
        ->create_volume(std::move(vol_info))
        .via(&folly::InlineExecutor::instance())
        .thenValue([](auto&& e) {
            if (!e.has_value()) { return std::make_error_condition(std::errc::io_error); }
            return std::error_condition();
        })
        .get();
}
#endif

// Return a device based on the format of the input
// Optional: pass in a unique identifier for metrics tracking
static std::unique_ptr< ublkpp::UblkDisk > get_driver(std::string const& resource, std::string const& metrics_id = "") {
#ifdef HAVE_HOMEBLOCKS
    if (0 < SISL_OPTIONS["homeblks_dev"].count()) {
        if (0 < SISL_OPTIONS["capacity"].count()) {
            try {
                auto const vol_uuid = boost::uuids::string_generator()(resource);
                if (!_app) {
                    if (_app = init_homeblocks(SISL_OPTIONS["homeblks_dev"].as< std::string >()); !_app) return nullptr;
                }
                if (auto e = create_hb_volume(*_app, vol_uuid); e) return nullptr;
                return std::make_unique< ublkpp::HomeBlkDisk >(vol_uuid, SISL_OPTIONS["capacity"].as< uint32_t >() * Gi,
                                                               _app->_hb->volume_manager(), 512 * Ki);
            } catch (std::runtime_error const& e) {}
        } else {
            LOGERROR("HomeBlocks device requires --capacity argument!")
        }
        return nullptr;
    }
#endif
    if (auto path = std::filesystem::path(resource); std::filesystem::exists(path)) {
        return std::make_unique< ublkpp::FSDisk >(path, metrics_id);
    }
#ifdef HAVE_ISCSI
    // From libiscsi.h iSCSI URLs are in the form:
    //   iscsi://[<username>[%<password>]@]<host>[:<port>]/<target-iqn>/<lun>
    return std::make_unique< ublkpp::iSCSIDisk >(resource);
#else
    return nullptr;
#endif
}

Result create_loop(boost::uuids::uuid const& id, std::string const& path) {
    auto dev = std::unique_ptr< ublkpp::UblkDisk >();
    try {
        auto loop_id = fmt::format("loop_{}", boost::uuids::to_string(id).substr(0, 8));
        dev = get_driver(path, loop_id);
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid0(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    auto dev = std::unique_ptr< ublkpp::Raid0Disk >();
    try {
        auto devices = std::vector< std::shared_ptr< ublkpp::UblkDisk > >();
        auto raid_uuid = boost::uuids::to_string(id);

        // Create stripe devices with RAID0 UUID for correlation
        for (auto const& disk : layout) {
            devices.push_back(get_driver(disk, raid_uuid));
        }

        if (0 < devices.size())
            dev = std::make_unique< ublkpp::Raid0Disk >(id, SISL_OPTIONS["stripe_size"].as< uint32_t >(),
                                                        std::move(devices));
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid1(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    auto dev = std::unique_ptr< ublkpp::Raid1Disk >();
    auto raid_uuid = boost::uuids::to_string(id);

    try {
        // Create FSDisk devices with RAID1 UUID for correlation
        auto dev_a = get_driver(*layout.begin(), raid_uuid);
        auto dev_b = get_driver(*(layout.begin() + 1), raid_uuid);

        dev = std::make_unique< ublkpp::Raid1Disk >(id, std::move(dev_a), std::move(dev_b), raid_uuid);
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

Result create_raid10(boost::uuids::uuid const& id, std::vector< std::string > const& layout) {
    if (1 > layout.size()) {
        LOGERROR("Zero mirrors in Array [uuid:{}]!", to_string(id))
        return std::unexpected(std::make_error_condition(std::errc::invalid_argument));
    }

    auto dev = std::unique_ptr< ublkpp::Raid0Disk >();
    auto raid10_uuid_str = boost::uuids::to_string(id);
    try {
        auto devices = std::vector< std::shared_ptr< ublkpp::UblkDisk > >();
        auto name_gen = boost::uuids::name_generator(id);

        // Process disks in pairs to create RAID1 mirrors
        for (size_t i = 0; i + 1 < layout.size(); i += 2) {
            // Generate partition UUID for this RAID1 mirror
            auto partition_uuid = name_gen(fmt::format("partition_{}", i / 2));
            auto partition_uuid_str = boost::uuids::to_string(partition_uuid);

            auto dev_a = get_driver(layout[i], partition_uuid_str);
            auto dev_b = get_driver(layout[i + 1], partition_uuid_str);

            // Create RAID1 mirror and add to devices
            devices.push_back(std::make_shared< ublkpp::Raid1Disk >(partition_uuid, std::move(dev_a), std::move(dev_b),
                                                                    raid10_uuid_str));
        }

        dev =
            std::make_unique< ublkpp::Raid0Disk >(id, SISL_OPTIONS["stripe_size"].as< uint32_t >(), std::move(devices));
    } catch (std::runtime_error const& e) {}
    if (!dev) return std::unexpected(std::make_error_condition(std::errc::operation_not_permitted));
    return _run_target(id, std::move(dev));
}

static void get_prometheus_metrics(const Pistache::Rest::Request&, Pistache::Http::ResponseWriter response) {
    response.send(Pistache::Http::Code::Ok, sisl::MetricsFarm::getInstance().report(sisl::ReportFormat::kTextFormat));
}

int main(int argc, char* argv[]) {
    SISL_OPTIONS_LOAD(argc, argv, ENABLED_OPTIONS);
    sisl::logging::SetLogger(std::string(argv[0]),
                             BOOST_PP_STRINGIZE(PACKAGE_NAME), BOOST_PP_STRINGIZE(PACKAGE_VERSION));
    spdlog::set_pattern("[%D %T] [%^%l%$] [%n] [%t] %v");
    ioenvironment.with_iomgr(iomgr::iomgr_params{.num_threads = 1}).with_http_server();

    signal(SIGINT, handle);
    signal(SIGTERM, handle);

    auto exit_future = s_stop_code.get_future();

    auto vol_id = (0 < SISL_OPTIONS["vol_id"].count())
        ? boost::uuids::string_generator()(SISL_OPTIONS["vol_id"].as< std::string >())
        : boost::uuids::random_generator()();
    Result res;
    if (0 < SISL_OPTIONS["loop"].count()) {
        res = create_loop(vol_id, SISL_OPTIONS["loop"].as< std::string >());
    } else if (0 < SISL_OPTIONS["raid0"].count()) {
        res = create_raid0(vol_id, SISL_OPTIONS["raid0"].as< std::vector< std::string > >());
    } else if (0 < SISL_OPTIONS["raid1"].count()) {
        res = create_raid1(vol_id, SISL_OPTIONS["raid1"].as< std::vector< std::string > >());
    } else if (0 < SISL_OPTIONS["raid10"].count()) {
        res = create_raid10(vol_id, SISL_OPTIONS["raid10"].as< std::vector< std::string > >());
    } else
        std::cout << SISL_PARSER.help({}) << std::endl;

    if (res) {

        // start the metrics server
        auto http_server_ptr = ioenvironment.get_http_server();
        try {
            auto routes = std::vector< iomgr::http_route >{{Pistache::Http::Method::Get, "/metrics",
                                                            Pistache::Rest::Routes::bind(get_prometheus_metrics),
                                                            iomgr::url_t::safe}};
            http_server_ptr->setup_routes(routes);
            http_server_ptr->start();
        } catch (std::runtime_error const& e) { LOGERROR("setup routes failed, {}", e.what()) }

        exit_future.wait();
        k_target->destroy();
        k_target.reset();
    } else
        s_stop_code.set_value(EIO);
#ifdef HAVE_HOMEBLOCKS
    if (_app) _app->_hb->shutdown();
#endif
    iomanager.stop();
    return exit_future.get();
}

static void handle(int signal) {
    switch (signal) {
    case SIGINT:
        [[fallthrough]];
    case SIGTERM: {
        // if multiple SIGTERM are received, the set_value will throw an exception
        try {
            LOGWARN("SIGNAL: {}", strsignal(signal));
            s_stop_code.set_value(signal);
        } catch (std::future_error const& e) { LOGERROR("Failed to set stop code: {}", e.what()); }
    } break;
        ;
    default:
        LOGERROR("Unhandled SIGNAL: {}", strsignal(signal));
        break;
    }
}
