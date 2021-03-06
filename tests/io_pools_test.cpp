#include <fstream>

#include <kora/dynamic.hpp>

#include <boost/program_options.hpp>

#define BOOST_TEST_NO_MAIN
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/included/unit_test.hpp>

#include "test_base.hpp"

using namespace tests;
namespace bu = boost::unit_test;

nodes_data::ptr configure_test_setup(const std::string &path) {
	auto server_config = []() {
		auto ret = server_config::default_value();
		ret.backends.clear();
		return ret;
	} ();

	start_nodes_config config(bu::results_reporter::get_stream(), {server_config}, path);
	config.fork = true;

	return start_nodes(config);
}

static uint32_t backend_to_group(uint32_t backend_id) {
	return 1000 + backend_id;
}

static void enable_backend(ioremap::elliptics::newapi::session &s, const nodes_data *setup, uint32_t backend_id) {
	auto remote = setup->nodes.front().remote();
	ELLIPTICS_REQUIRE(async, s.enable_backend(remote, backend_id));
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);
}

static void disable_backend(ioremap::elliptics::newapi::session &s, const nodes_data *setup, uint32_t backend_id) {
	auto remote = setup->nodes.front().remote();
	ELLIPTICS_REQUIRE(async, s.disable_backend(remote, backend_id));
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);
}

static void remove_backend(ioremap::elliptics::newapi::session &s, const nodes_data *setup, uint32_t backend_id) {
	auto remote = setup->nodes.front().remote();
	ELLIPTICS_REQUIRE(async, s.remove_backend(remote, backend_id));
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);
}

static void check_statistics(ioremap::elliptics::newapi::session &s,
                             std::vector<std::tuple<uint32_t, std::string>> enabled_backends,
                             std::vector<std::tuple<uint32_t, std::string>> disabled_backends) {
	if (enabled_backends.empty())
		// routes should be empty if no backends are enabled
		BOOST_REQUIRE_EQUAL(s.get_routes().size(), 0);

	ELLIPTICS_REQUIRE(async, s.monitor_stat(DNET_MONITOR_IO | DNET_MONITOR_BACKEND));
	auto results = async.get();
	BOOST_REQUIRE_EQUAL(results.size(), 1); // the only node is run

	auto statistics = [&results]() {
		auto json = results[0].statistics();
		std::istringstream stream(json);
		return kora::dynamic::read_json(stream);
	}();
	BOOST_REQUIRE(statistics.is_object());

	const auto &backends = statistics.as_object()["backends"];
	BOOST_REQUIRE(backends.is_object());
	BOOST_REQUIRE_EQUAL(backends.as_object().size(), enabled_backends.size() + disabled_backends.size());

	auto check_backend = [&backends](const std::tuple<uint32_t, std::string> tuple, bool enabled) {
		const auto backend_id = std::get<0>(tuple);
		const auto &pool_id = std::get<1>(tuple).empty() ? std::to_string(backend_id) : std::get<1>(tuple);

		const auto &backend = backends.as_object()[std::to_string(backend_id)];
		BOOST_REQUIRE(backend.is_object());
		BOOST_REQUIRE_EQUAL(backend.as_object()["backend_id"].as_uint(), backend_id);

		const auto &status = backend.as_object()["status"];
		BOOST_REQUIRE(status.is_object());
		BOOST_REQUIRE_EQUAL(status.as_object()["backend_id"].as_uint(), backend_id);
		BOOST_REQUIRE_EQUAL(status.as_object()["state"].as_uint(),
		                    enabled ? DNET_BACKEND_ENABLED : DNET_BACKEND_DISABLED);
		BOOST_REQUIRE_EQUAL(status.as_object()["pool_id"].as_string(), enabled ? pool_id : "");
		BOOST_REQUIRE_EQUAL(status.as_object()["group"].as_uint(), backend_to_group(backend_id));

		if (!enabled) {
			BOOST_REQUIRE(backend.as_object().find("io") == backend.as_object().end());
			return;
		}

		const auto &io = backend.as_object()["io"];
		BOOST_REQUIRE(io.is_object());

		const auto &blocking = io.as_object()["blocking"];
		BOOST_REQUIRE(blocking.is_object());
		BOOST_REQUIRE_EQUAL(blocking.as_object()["current_size"].as_uint(), 0);

		const auto &nonblocking = io.as_object()["nonblocking"];
		BOOST_REQUIRE(nonblocking.is_object());
		BOOST_REQUIRE_EQUAL(nonblocking.as_object()["current_size"].as_uint(), 0);
	};

	for (const auto &tuple : enabled_backends) {
		check_backend(tuple, true);
	}

	for (const auto &tuple : disabled_backends) {
		check_backend(tuple, false);
	}

	const auto &io = statistics.as_object()["io"];
	BOOST_REQUIRE(io.is_object());

	const auto &pools_stats = io.as_object()["pools"];
	BOOST_REQUIRE(pools_stats.is_object());

	auto pools = [&enabled_backends]() {
		std::unordered_set<std::string> ret;
		for (const auto &tuple : enabled_backends) {
			const auto &pool_id = std::get<1>(tuple);
			if (!pool_id.empty())
				ret.emplace(pool_id);
		}
		return std::move(ret);
	}();
	BOOST_REQUIRE_EQUAL(pools_stats.as_object().size(), pools.size());

	for (const auto &pool_id: pools) {
		const auto &pool = pools_stats.as_object()[pool_id];
		BOOST_REQUIRE(pool.is_object());
		{
			const auto &blocking = pool.as_object()["blocking"];
			BOOST_REQUIRE(blocking.is_object());
			BOOST_REQUIRE_EQUAL(blocking.as_object()["current_size"].as_uint(), 0);

			const auto &nonblocking = pool.as_object()["nonblocking"];
			BOOST_REQUIRE(nonblocking.is_object());
			BOOST_REQUIRE_EQUAL(nonblocking.as_object()["current_size"].as_uint(), 0);
		}
	}
}

static void test_empy_node(ioremap::elliptics::newapi::session &s) {
	check_statistics(s, {}, {});
}

static void add_backends_to_config(const nodes_data *setup,
                                   std::vector<std::tuple<uint32_t, std::string>> backends_to_add) {
	auto config_path = setup->nodes.front().config_path();

	auto config = [&config_path]() {
		// read and parse server config
		std::ifstream stream(config_path);
		return kora::dynamic::read_json(stream);
	}();
	auto &backends = config.as_object()["backends"];

	BOOST_REQUIRE(backends.is_array());
	BOOST_REQUIRE_EQUAL(backends.as_array().size(), 0);

	for (const auto &backend_to_add : backends_to_add) {
		const auto &backend_id = std::get<0>(backend_to_add);
		const auto &pool_id = std::get<1>(backend_to_add);

		// prepare directory for the backend
		std::string prefix =
		        config_path.substr(0, config_path.find_last_of('/')) + '/' + std::to_string(backend_id);
		create_directory(prefix);
		create_directory(prefix + "/history");
		create_directory(prefix + "/blob");

		// add backend with individual pool
		kora::dynamic_t::object_t backend;
		backend["type"] = "blob";
		backend["backend_id"] = backend_id;
		backend["history"] = prefix + "/history";
		backend["data"] = prefix + "/blob";
		backend["group"] = backend_to_group(backend_id);
		if (!pool_id.empty())
			backend["pool_id"] = pool_id;
		backends.as_array().emplace_back(std::move(backend));
	}

	std::ofstream stream(config_path);
	kora::write_pretty_json(stream, config);
}

static void test_one_backend(ioremap::elliptics::newapi::session &s,
                             const nodes_data *setup,
                             const std::tuple<uint32_t, std::string> &backend) {
	// add backend with @backend_id and individual pool to config
	add_backends_to_config(setup, {backend});
	// nothing should happen after config update
	check_statistics(s, {}, {});

	// repeat test's body 20 times
	// for (size_t i = 0; i < 20; ++i) {
		// make enabled -> disable -> remove
		enable_backend(s, setup, std::get<0>(backend));
		check_statistics(s, {backend}, {});

		disable_backend(s, setup, std::get<0>(backend));
		check_statistics(s, {}, {backend});

		remove_backend(s, setup, std::get<0>(backend));
		check_statistics(s, {}, {});

		// make enabled -> remove
		enable_backend(s, setup, std::get<0>(backend));
		check_statistics(s, {backend}, {});

		remove_backend(s, setup, std::get<0>(backend));
		check_statistics(s, {}, {});
	// }

	// revert on-disk config to original one
	setup->nodes.front().config().write(setup->nodes.front().config_path());
}

typedef std::array<std::tuple<uint32_t, std::string>, 2> two_backends_array;

static void test_two_backends(ioremap::elliptics::newapi::session &s,
                              const nodes_data *setup,
                              two_backends_array backends) {
	// add backend with @backend_id and shared pool to config
	add_backends_to_config(setup, {backends[0], backends[1]});
	// nothing should happen after config update
	check_statistics(s, {}, {});

	// enable one by one backends

	enable_backend(s, setup, std::get<0>(backends[0]));
	check_statistics(s, {backends[0]}, {});

	enable_backend(s, setup, std::get<0>(backends[1]));
	check_statistics(s, {backends[0], backends[1]}, {});

	// disable one by one backends

	disable_backend(s, setup, std::get<0>(backends[0]));
	check_statistics(s, {backends[1]}, {backends[0]});

	disable_backend(s, setup, std::get<0>(backends[1]));
	check_statistics(s, {}, {backends[0], backends[1]});

	// remove one by one backends

	remove_backend(s, setup, std::get<0>(backends[0]));
	check_statistics(s, {}, {backends[1]});

	remove_backend(s, setup, std::get<0>(backends[1]));
	// after remove node should be returned to original state
	check_statistics(s, {}, {});

	// revert on-disk config to original one
	setup->nodes.front().config().write(setup->nodes.front().config_path());
}

static void test_backends_with_delay(ioremap::elliptics::newapi::session &s, const nodes_data *setup) {
	two_backends_array backends = {std::make_tuple(1, "pool"), std::make_tuple(2, "pool")};
	const auto delayed_backend_id = std::get<0>(backends[0]);
	const auto normal_backend_id = std::get<0>(backends[1]);

	auto read_from_backend = [&](uint32_t backend_id) {
		const int group = backend_to_group(backend_id);
		auto read_session = s.clone();
		read_session.set_groups({group});
		read_session.set_timeout(1);
		return read_session.read_data({"non-existent-key"}, 0, 0);
	};

	// add 2 backends with a shared pool to config
	add_backends_to_config(setup, {backends[0], backends[1]});

	// enable both backend
	enable_backend(s, setup, std::get<0>(backends[0]));
	enable_backend(s, setup, std::get<0>(backends[1]));

	// check that both backends are enabled
	check_statistics(s, {backends[0], backends[1]}, {});

	// set 2 second delay to first backend
	s.set_delay(setup->nodes.front().remote(), delayed_backend_id, 2000).wait();

	{ // check delay
		auto async = read_from_backend(delayed_backend_id);
		async.wait();
		BOOST_REQUIRE_EQUAL(async.error().code(), -ETIMEDOUT);
	}

	{ // asynchronously send command to both backends and check that second backend will not timeout
		auto delayed_async = read_from_backend(delayed_backend_id);
		auto normal_async = read_from_backend(normal_backend_id);
		delayed_async.wait();
		normal_async.wait();
		BOOST_REQUIRE_EQUAL(delayed_async.error().code(), -ETIMEDOUT);
		BOOST_REQUIRE_EQUAL(normal_async.error().code(), -ENOENT);
	}

	{ // asynchronously send bulk_read to delayed backend, remove it and check that removing waits bulk_read's
	  // completion
		auto read_session = s.clone();
		read_session.set_timeout(1);
		// generate ids for bulk read
		auto ids = [&]() {
			dnet_id id;
			read_session.transform("non-existent-key", id);
			id.group_id = backend_to_group(delayed_backend_id);
			return std::vector<dnet_id>(1000, id); // use one key 1000 times
		}();
		auto delayed_async = read_session.bulk_read(ids);
		remove_backend(s, setup, std::get<0>(backends[0]));
		// remove_backend should wait bulk_read completion, so when it is completed
		// bulk_read should also be completed.
		BOOST_REQUIRE(delayed_async.ready());
	}

	// remove the remaining backend
	remove_backend(s, setup, std::get<0>(backends[1]));
	// check original state of statistics
	check_statistics(s, {}, {});

	// revert on-disk config to original one
	setup->nodes.front().config().write(setup->nodes.front().config_path());
}

bool register_tests(const nodes_data *setup) {
	auto n = setup->node->get_native();

	// Test node without enabled backends after start-up
	ELLIPTICS_TEST_CASE(test_empy_node, use_session(n));

	// Test node with one backend

	// one backend with individual pool
	ELLIPTICS_TEST_CASE(test_one_backend, use_session(n), setup, std::make_tuple(1, ""));
	// one backend with shared pool
	ELLIPTICS_TEST_CASE(test_one_backend, use_session(n), setup, std::make_tuple(1, "bla"));

	// Test node with two backends

	// two backends with one shared pool
	ELLIPTICS_TEST_CASE(test_two_backends, use_session(n), setup,
	                    two_backends_array{std::make_tuple(1, "bla"), std::make_tuple(2, "bla")});
	// two backends with two different shared pool
	ELLIPTICS_TEST_CASE(test_two_backends, use_session(n), setup,
	                    two_backends_array{std::make_tuple(1, "bla1"), std::make_tuple(2, "bla2")});
	// one backend with shared pool and one with individual
	ELLIPTICS_TEST_CASE(test_two_backends, use_session(n), setup,
	                    two_backends_array{std::make_tuple(1, "bla1"), std::make_tuple(2, "")});

	// test affecting backend's delay on other backends which share the same pool
	ELLIPTICS_TEST_CASE(test_backends_with_delay, use_session(n), setup);

	return true;
}

tests::nodes_data::ptr configure_test_setup_from_args(int argc, char *argv[]) {
	namespace bpo = boost::program_options;

	bpo::variables_map vm;
	bpo::options_description generic("Test options");

	std::string path;

	generic.add_options()
		("help", "This help message")
		("path", bpo::value(&path), "Path where to store everything")
		;

	bpo::store(bpo::parse_command_line(argc, argv, generic), vm);
	bpo::notify(vm);

	if (vm.count("help")) {
		std::cerr << generic;
		return nullptr;
	}

	return configure_test_setup(path);
}

/*
 * Common test initialization routine.
 */
using namespace tests;
using namespace boost::unit_test;

/*FIXME: forced to use global variable and plain function wrapper
 * because of the way how init_test_main works in boost.test,
 * introducing a global fixture would be a proper way to handle
 * global test setup
 */
namespace {
std::shared_ptr<nodes_data> setup;
bool init_func() {
	return register_tests(setup.get());
}
}

int main(int argc, char *argv[]) {
	srand(time(nullptr));

	// we own our test setup
	setup = configure_test_setup_from_args(argc, argv);

	int result = unit_test_main(init_func, argc, argv);

	// disassemble setup explicitly, to be sure about where its lifetime ends
	setup.reset();

	return result;
}

