#include <fstream>
#include <boost/program_options.hpp>

#define BOOST_TEST_NO_MAIN
#define BOOST_TEST_ALTERNATIVE_INIT_API
#include <boost/test/included/unit_test.hpp>

#include <eblob/blob.h>
#include "library/common.hpp"
#include "elliptics/newapi/session.hpp"

#include "test_base.hpp"

namespace tests {

namespace bu = boost::unit_test;

const std::vector<int> groups{1,2,3};

nodes_data::ptr configure_test_setup(const std::string &path) {
	auto server_config = [](const std::vector<int> &groups) {
		auto ret = server_config::default_value();
		ret.backends.resize(groups.size(), ret.backends.front());
		for (size_t i = 0; i < groups.size(); ++i) {
			ret.backends[i]("group", groups[i]);
		}
		return ret;
	};

	/* Create 3 server nodes each containing two groups.
	 * Groups 1, 2, 3 are used in all tests, while 4, 5, 6 are bulk_read-specific.
	 */
	auto configs = {server_config({1, 4}),
	                server_config({2, 5}),
	                server_config({3, 6})};

	start_nodes_config config(bu::results_reporter::get_stream(), configs, path);
	config.fork = true;

	return start_nodes(config);
}

struct record {
	ioremap::elliptics::key key;
	uint64_t user_flags;
	dnet_time timestamp;
	dnet_time json_timestamp;
	std::string json;
	uint64_t json_capacity;
	std::string data;
	uint64_t data_capacity;
	bool in_cache;
};

void check_lookup_result(ioremap::elliptics::newapi::async_lookup_result &async,
                         const int command,
                         const record &record,
                         const size_t expected_count) {
	size_t count = 0;
	for (const auto &result: async) {
		(void)result.address();
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, command);
		BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		BOOST_REQUIRE_EQUAL(result.error().message(), "");

		auto record_info = result.record_info();

		BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);

		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.record_flags,
			                    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
		if (!record.in_cache) {
			constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
			BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
		}
		BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());

		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + record.json_capacity);
		}
		BOOST_REQUIRE_EQUAL(record_info.data_size, record.data.size());

		if (!record.in_cache) {
			std::ifstream blob(result.path(), std::ifstream::binary);
			BOOST_REQUIRE(blob);
			if (record.json.size())
			{
				blob.seekg(record_info.json_offset);
				auto buffer = ioremap::elliptics::data_pointer::allocate(record.json.size());
				blob.read(buffer.data<char>(), buffer.size());
				BOOST_REQUIRE(blob);
				BOOST_REQUIRE_EQUAL(buffer.to_string(), record.json);
			}

			if (record.data.size()) {
				blob.seekg(record_info.data_offset);
				auto buffer = ioremap::elliptics::data_pointer::allocate(record.data.size());
				blob.read(buffer.data<char>(), buffer.size());
				BOOST_REQUIRE(blob);
				BOOST_REQUIRE_EQUAL(buffer.to_string(), record.data);
			}
		}

		++count;
	}

	BOOST_REQUIRE_EQUAL(count, expected_count);
}

} // namespace tests

namespace all {

using namespace tests;

void test_write(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_user_flags(record.user_flags);

	s.set_timestamp(record.timestamp);
	s.set_json_timestamp(record.json_timestamp);

	auto async = s.write(record.key,
	                     record.json, record.json_capacity,
	                     record.data, record.data_capacity);

	check_lookup_result(async, DNET_CMD_WRITE_NEW, record, groups.size());
}

void test_update_json(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_user_flags(record.user_flags);

	s.set_json_timestamp(record.json_timestamp);

	auto async = s.update_json(record.key, record.json);

	check_lookup_result(async, DNET_CMD_WRITE_NEW, record, groups.size());
}

void test_update_bigger_json(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	/* generate json bigger than record.json_capacity
	 */
	auto big_json = [&record] () {
		std::ostringstream str;
		str << "{\"big_key\":\"";
		while (str.tellp() < (off_t)record.json_capacity) {
			str << "garbage";
		}
		str << "\"}";
		return str.str();
	} ();

	auto async = s.update_json(record.key, big_json);
	size_t count = 0;
	for (const auto &result: async) {
		BOOST_REQUIRE_EQUAL(result.status(), -E2BIG);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		++count;
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_update_json_noexist(const ioremap::elliptics::newapi::session &session) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	auto async = s.update_json(std::string{"test_update_json_noexist key"}, std::string{"{}"});

	size_t count = 0;
	for (const auto &result: async) {
		BOOST_REQUIRE_EQUAL(result.status(), -ENOENT);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		++count;
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_update_json_uncommitted(const ioremap::elliptics::newapi::session &session) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	std::string key{"test_update_json_uncommitted key"};

	auto async = s.write_prepare(key, "", 1024, "", 0, 1024);
	async.wait();

	async = s.update_json(key, std::string{"{}"});
	size_t count = 0;
	for (const auto &result: async) {
		BOOST_REQUIRE_EQUAL(result.status(), -ENOENT);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		++count;
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_lookup(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();
	s.set_groups(groups);

	auto async = s.lookup(record.key);

	check_lookup_result(async, DNET_CMD_LOOKUP_NEW, record, 1);
}

void test_read_json(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();

	size_t count = 0;

	for (const auto &group : groups) {
		s.set_groups({group});
		auto async = s.read_json(record.key);

		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto info = result.record_info();

			BOOST_REQUIRE_EQUAL(info.user_flags, record.user_flags);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(info.record_flags,
						    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&info.json_timestamp, &record.json_timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(info.json_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(info.json_size, record.json.size());
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(info.json_capacity, record.json_capacity);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&info.data_timestamp, &record.timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(info.data_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(info.data_size, record.data.size());

			BOOST_REQUIRE_EQUAL(result.json().to_string(), record.json);
			BOOST_REQUIRE(result.data().empty());

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, record.json.size());
			BOOST_REQUIRE_EQUAL(io_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_size, 0);

			++count;
		}
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_read_data(const ioremap::elliptics::newapi::session &session, const record &record, uint64_t offset,
                    uint64_t size) {
	auto s = session.clone();

	size_t count = 0;

	for (const auto &group : groups) {
		s.set_groups({group});
		auto async = s.read_data(record.key, offset, size);

		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags,
						    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, record.data.size());

			BOOST_REQUIRE(result.json().empty());
			auto data_part = record.data.substr(offset, size ? size : std::string::npos);
			BOOST_REQUIRE_EQUAL(result.data().to_string(), data_part);

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_offset, offset);
			BOOST_REQUIRE_EQUAL(io_info.data_size, data_part.size());

			++count;
		}
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_read(const ioremap::elliptics::newapi::session &session, const record &record, uint64_t offset,
               uint64_t size) {
	auto s = session.clone();

	size_t count = 0;

	for (const auto &group : groups) {
		s.set_groups({group});
		auto async = s.read(record.key, offset, size);

		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags,
						    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
			if (!record.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, record.data.size());

			BOOST_REQUIRE_EQUAL(result.json().to_string(), record.json);
			auto data_part = record.data.substr(offset, size ? size : std::string::npos);
			BOOST_REQUIRE_EQUAL(result.data().to_string(), data_part);

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, record.json.size());
			BOOST_REQUIRE_EQUAL(io_info.data_offset, offset);
			BOOST_REQUIRE_EQUAL(io_info.data_size, data_part.size());

			++count;
		}
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_write_chunked(const ioremap::elliptics::newapi::session &session, const record &record) {
	auto s = session.clone();
	s.set_groups(groups);
	s.set_user_flags(record.user_flags);
	s.set_timestamp(record.timestamp);
	s.set_json_timestamp(record.json_timestamp);

	auto async = s.write_prepare(record.key,
	                             std::string{}, record.json_capacity,
	                             record.data, 0, record.data_capacity);

	size_t count = 0;
	for (const auto &result: async) {
		(void)result.address();
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		BOOST_REQUIRE_EQUAL(result.error().message(), "");

		auto record_info = result.record_info();

		BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.record_flags, DNET_RECORD_FLAGS_EXTHDR |
		                                                      DNET_RECORD_FLAGS_CHUNKED_CSUM |
		                                                      DNET_RECORD_FLAGS_UNCOMMITTED);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
		if (!record.in_cache) {
			constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
			BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
		}
		BOOST_REQUIRE_EQUAL(record_info.json_size, 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + record.json_capacity);
			BOOST_REQUIRE_EQUAL(record_info.data_size, 0);
		}
		++count;
	}
	BOOST_REQUIRE_EQUAL(count, groups.size());

	async = s.write_plain(record.key,
	                      record.json,
	                      std::string{}, 0);

	count = 0;
	for (const auto &result: async) {
		(void)result.address();
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		BOOST_REQUIRE_EQUAL(result.error().message(), "");

		auto record_info = result.record_info();

		BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.record_flags, DNET_RECORD_FLAGS_EXTHDR |
		                                                      DNET_RECORD_FLAGS_CHUNKED_CSUM |
		                                                      DNET_RECORD_FLAGS_UNCOMMITTED);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
		if (!record.in_cache) {
			constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
			BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
		}
		BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + record.json_capacity);
			BOOST_REQUIRE_EQUAL(record_info.data_size, 0);
		}
		++count;
	}
	BOOST_REQUIRE_EQUAL(count, groups.size());

	async = s.write_plain(record.key,
	                      std::string{},
	                      record.data, record.data.size());

	count = 0;
	for (const auto &result: async) {
		(void)result.address();
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		BOOST_REQUIRE_EQUAL(result.error().message(), "");

		auto record_info = result.record_info();

		BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.record_flags, DNET_RECORD_FLAGS_EXTHDR |
		                                                      DNET_RECORD_FLAGS_CHUNKED_CSUM |
		                                                      DNET_RECORD_FLAGS_UNCOMMITTED);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
		if (!record.in_cache) {
			constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
			BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
		}
		BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + record.json_capacity);
			BOOST_REQUIRE_EQUAL(record_info.data_size, 0);
		}
		++count;
	}
	BOOST_REQUIRE_EQUAL(count, groups.size());

	async = s.write_commit(record.key,
	                       record.json,
	                       record.data, 2*record.data.size(), 3*record.data.size());

	count = 0;
	for (const auto &result: async) {
		(void)result.address();
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
		BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		BOOST_REQUIRE_EQUAL(result.error().message(), "");

		auto record_info = result.record_info();

		BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.record_flags,
					    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
		if (!record.in_cache) {
			constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
			BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
		}
		BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);
		}

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
		if (!record.in_cache) {
			BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + record.json_capacity);
		}
		BOOST_REQUIRE_EQUAL(record_info.data_size, 3*record.data.size());
		++count;
	}
	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_old_write_new_read_compatibility(const ioremap::elliptics::newapi::session &session) {
	static const ioremap::elliptics::key key{"test_old_write_new_read_compatibility's key"};
	static const std::string data{"test_old_write_new_read_compatibility's data"};
	constexpr uint64_t user_flags = 0xfc1234;
	constexpr dnet_time timestamp{1, 2};
	constexpr dnet_time empty_time{0, 0};
	constexpr uint64_t record_flags = DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM;
	constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);
	{
		ioremap::elliptics::session s(session.get_native_node());
		s.set_groups(groups);
		s.set_user_flags(user_flags);
		s.set_ioflags(session.get_ioflags());
		s.set_timestamp(timestamp);

		auto async = s.write_data(key, data, 0);

		size_t count = 0;
		for (const auto &result: async) {
			(void)result.storage_address();

			auto file_info = result.file_info();
			BOOST_REQUIRE(file_info != nullptr);

			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(file_info->record_flags, record_flags);
			}
			BOOST_REQUIRE_EQUAL(file_info->size, data.size());
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE(file_info->offset >= eblob_headers_size);
			}
			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&file_info->mtime, &timestamp), 0);
			++count;
		}

		BOOST_REQUIRE_EQUAL(count, groups.size());
	}

	{
		auto s = session.clone();
		s.set_groups(groups);

		auto async = s.lookup(key);

		size_t count = 0;
		for (const auto &result: async) {
			(void)result.address();

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, user_flags);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags, record_flags);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &empty_time), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, 0);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &timestamp), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, data.size());

			++count;
		}

		BOOST_REQUIRE_EQUAL(count, 1);
	}

	{
		auto s = session.clone();
		s.set_groups(groups);

		auto async = s.read_json(key);

		size_t count = 0;
		for (const auto &result: async) {
			(void)result.address();

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, user_flags);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags, record_flags);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &empty_time), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, 0);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &timestamp), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, data.size());

			BOOST_REQUIRE(result.json().empty());
			BOOST_REQUIRE(result.data().empty());

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_size, 0);

			++count;
		}

		BOOST_REQUIRE_EQUAL(count, 1);
	}

	{
		auto s = session.clone();
		s.set_groups(groups);

		auto async = s.read(key, 0, 0);

		size_t count = 0;
		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, user_flags);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags, record_flags);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &empty_time), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, 0);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &timestamp), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, data.size());

			BOOST_REQUIRE(result.json().empty());
			BOOST_REQUIRE_EQUAL(result.data().to_string(), data);

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_size, data.size());

			++count;
		}

		BOOST_REQUIRE_EQUAL(count, 1);
	}
}

void test_new_write_old_read_compatibility(const ioremap::elliptics::newapi::session &session) {
	static const ioremap::elliptics::key key{"test_new_write_old_read_compatibility's key"};
	static const std::string json{"{\"some_field\":\"some_field's data\"}"};
	uint64_t json_capacity = 100;
	static const std::string data{"test_new_write_old_read_compatibility's data"};
	uint64_t data_capacity = 200;
	constexpr uint64_t user_flags = 0xfc1234;
	constexpr dnet_time timestamp{1, 2};
	constexpr uint64_t record_flags = DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM;
	constexpr uint64_t eblob_headers_size = sizeof(eblob_disk_control) + sizeof(dnet_ext_list_hdr);

	{
		auto s = session.clone();
		s.set_groups(groups);
		s.set_user_flags(user_flags);

		s.set_timestamp(timestamp);

		auto async = s.write(key,
		                     json, json_capacity,
		                     data, data_capacity);
		size_t count = 0;
		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_WRITE_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, user_flags);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.record_flags, record_flags);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &timestamp), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE(record_info.json_offset >= eblob_headers_size);
			}
			BOOST_REQUIRE_EQUAL(record_info.json_size, json.size());
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, json_capacity);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &timestamp), 0);
			if (!(s.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(record_info.data_offset, record_info.json_offset + json_capacity);
			}
			BOOST_REQUIRE_EQUAL(record_info.data_size, data.size());
			++count;
		}

		BOOST_REQUIRE_EQUAL(count, groups.size());
	}

	{
		ioremap::elliptics::session s(session.get_native_node());
		s.set_groups(groups);

		auto async = s.lookup(key);

		size_t count = 0;
		for (const auto &result: async) {
			// (void)result.storage_address();

			auto file_info = result.file_info();
			BOOST_REQUIRE(file_info != nullptr);

			if (!(session.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(file_info->record_flags, record_flags);
			}
			BOOST_REQUIRE_EQUAL(file_info->size, data.size());
			BOOST_REQUIRE(file_info->offset >= eblob_headers_size);
			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&file_info->mtime, &timestamp), 0);

			++count;
		}

		BOOST_REQUIRE_EQUAL(count, 1);
	}

	{
		ioremap::elliptics::session s(session.get_native_node());
		s.set_groups(groups);

		auto async = s.read_data(key, 0, 0);

		size_t count = 0;
		for (const auto &result: async) {
			BOOST_REQUIRE_EQUAL(result.file().to_string(), data);

			auto io = result.io_attribute();

			BOOST_REQUIRE(io != nullptr);
			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&io->timestamp, &timestamp), 0);
			BOOST_REQUIRE_EQUAL(io->user_flags, user_flags);
			BOOST_REQUIRE_EQUAL(io->total_size, data.size());
			if (!(session.get_ioflags() & DNET_IO_FLAGS_CACHE)) {
				BOOST_REQUIRE_EQUAL(io->record_flags, record_flags);
			}
			BOOST_REQUIRE_EQUAL(io->offset, 0);
			BOOST_REQUIRE_EQUAL(io->size, data.size());

			++count;
		}

		BOOST_REQUIRE_EQUAL(count, 1);
	}
}

void corrupt_record(const std::string &path, off_t offset, const std::string &injection) {
	int fd = open(path.c_str(), O_RDWR, 0644);

	BOOST_REQUIRE(fd > 0);
	BOOST_REQUIRE_EQUAL(pwrite(fd, injection.c_str(), injection.size(), offset), injection.size());

	close(fd);
}

void write_and_corrupt_record(ioremap::elliptics::newapi::session &s, const ioremap::elliptics::key &key,
                              const std::string &json, uint64_t json_capacity,
                              const std::string &data, uint64_t data_capacity,
                              uint64_t injection_offset) {
	auto async = s.write(key, json, json_capacity, data, data_capacity);

	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result =  async.get()[0];

	corrupt_record(result.path(), result.record_info().json_offset + injection_offset, "asjdhfpapof");
}

void write_and_corrupt_json(ioremap::elliptics::newapi::session &s, const ioremap::elliptics::key &key,
                            const std::string &json, uint64_t json_capacity,
                            const std::string &data, uint64_t data_capacity,
                            uint64_t injection_offset=0) {
	write_and_corrupt_record(s, key, json, json_capacity, data, data_capacity, injection_offset);
}

void write_and_corrupt_data(ioremap::elliptics::newapi::session &s, const ioremap::elliptics::key &key,
                            const std::string &json, uint64_t json_capacity,
                            const std::string &data, uint64_t data_capacity) {
	write_and_corrupt_record(s, key, json, json_capacity, data, data_capacity, json_capacity);
}

void test_read_corrupted_json(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_corrupted_json key"};
	static const std::string data{"write_and_corrupt_json data"};
	static const std::string json{R"json(
	{
		"key": "write_and_corrupt_json json key"
	}
	)json"};
	write_and_corrupt_json(s, key, json, 0, data, 0);

	auto async = s.read_json(key);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
}

void test_read_json_with_corrupted_data_part(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_json_with_corrupted_data_part key"};
	static const std::string data{"test_read_json_with_corrupted_data_part data"};
	static const std::string json{R"json(
	{
		"key": "test_read_json_with_corrupted_data_part json key"
	}
	)json"};

	write_and_corrupt_data(s, key, json, 0, data, 0);

	auto async = s.read_json(key);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
}

void test_read_json_with_big_capacity_and_corrupted_data_part(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});

	static const std::string key{"test_read_json_with_big_capacity_and_corrupted_data_part key"};
	static const std::string data{"test_read_json_with_big_capacity_and_corrupted_data_part data"};
	static const std::string json{R"json(
	{
		"key": "test_read_json_with_big_capacity_and_corrupted_data_part json"
	}
	)json"};

	write_and_corrupt_data(s, key, json, 1<<20, data, 0);

	auto async = s.read_json(key);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];

	BOOST_REQUIRE_EQUAL(result.json().to_string(), json);
}

void test_read_data_with_corrupted_json(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_data_with_corrupted_json key"};
	static const std::string data{"test_read_data_with_corrupted_json data"};
	static const std::string json{R"json(
	{
		"key": "test_read_data_with_corrupted_json json"
	}
	)json"};

	write_and_corrupt_json(s, key, json, 0, data, 0);

	auto async = s.read_data(key, 0, 0);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
}

void test_read_data_with_corrupted_json_with_big_capacity(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});

	static const std::string key{"test_read_data_with_corrupted_json_with_big_capacity key"};
	static const std::string data{"test_read_data_with_corrupted_json_with_big_capacity data"};
	static const std::string json{R"json(
	{
		"key": "test_read_data_with_corrupted_json_with_big_capacity json"
	}
	)json"};

	write_and_corrupt_json(s, key, json, 2 << 20 /*json_capacity*/, data, 0 /*data_capacity */,
	                       1 << 20 /*injection_offset*/);

	auto async = s.read_data(key, 0, 0);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];

	BOOST_REQUIRE_EQUAL(result.data().to_string(), data);
}

void test_read_data_with_corrupted_data(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_data_with_corrupted_data key"};
	static const std::string data{"test_read_data_with_corrupted_data data"};
	static const std::string json{R"json(
	{
		"key": "test_read_data_with_corrupted_data json"
	}
	)json"};

	write_and_corrupt_data(s, key, json, 0, data, 0);

	auto async = s.read_data(key, 0, 0);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
}

std::string make_data(const std::string &pattern, off_t size) {
	std::ostringstream str;
	while (str.tellp() < size) {
		str << pattern;
	}
	return str.str();
}

void test_read_data_part_with_corrupted_first_data(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_data_part_with_corrupted_first_data key"};
	static const std::string data = make_data({"test_read_first_data_with_corrupted_first_data"}, 2 << 20);
	static const std::string json{R"json(
	{
		"key": "test_read_data_part_with_corrupted_first_data json"
	}
	)json"};

	std::cout << "data: " << data.size() << std::endl;

	write_and_corrupt_record(s, key, json, 1 << 20 /*json_capacity*/, data, 0 /*data_capacity*/,
	                         1 << 20 /*injection_offset*/);

	auto async = s.read_data(key, 0, 0);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);

	async = s.read_data(key, 1 << 20, 100);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), 0);
	const auto data_part = data.substr(1 << 20, 100);
	BOOST_REQUIRE_EQUAL(result.data().to_string(), data_part);
}

void test_read_data_part_with_corrupted_second_data(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_read_data_part_with_corrupted_second_data key"};
	static const std::string data = make_data({"test_read_first_data_with_corrupted_first_data"}, 2<<20);
	static const std::string json{R"json(
	{
		"key": "test_read_data_part_with_corrupted_second_data json"
	}
	)json"};

	write_and_corrupt_record(s, key, json, 0, data, 0, json.size() + (1<<20));

	auto async = s.read_data(key, 0, 100);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), 0);
	auto data_part = data.substr(0, 100);
	BOOST_REQUIRE_EQUAL(result.data().to_string(), data_part);

	async = s.read_data(key, 1<<20, 0);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
}

void test_data_and_json_timestamp(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	static const ioremap::elliptics::key key{"test_data_and_json_timestamp's key"};
	static const std::string json{"{\"some_field\":\"some_field's data\"}"};
	uint64_t json_capacity = 100;
	static const std::string data{"test_data_and_json_timestamp's data"};
	uint64_t data_capacity = 200;
	constexpr dnet_time data_timestamp{1, 2};
	constexpr dnet_time json_timestamp{3, 4};
	dnet_time empty_timestamp;
	dnet_empty_time(&empty_timestamp);

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	auto write = [&] () {
		auto async = s.write(key, json, json_capacity, data, data_capacity);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), 0);
		return result.record_info();
	};

	{
		s.set_timestamp(data_timestamp);

		auto record_info = write();

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &data_timestamp), 0);
		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &data_timestamp), 0);
	}

	{
		s.set_timestamp(empty_timestamp);
		s.set_json_timestamp(json_timestamp);

		auto record_info = write();

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &json_timestamp), 0);
		BOOST_REQUIRE_NE(dnet_time_cmp(&record_info.data_timestamp, &empty_timestamp), 0);
		BOOST_REQUIRE_NE(dnet_time_cmp(&record_info.data_timestamp, &record_info.json_timestamp), 0);
	}

	{
		s.set_timestamp(empty_timestamp);
		s.reset_json_timestamp();

		auto record_info = write();

		BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record_info.data_timestamp), 0);
		BOOST_REQUIRE_NE(dnet_time_cmp(&record_info.data_timestamp, &empty_timestamp), 0);
	}
}

void test_write_plain_into_nonexistent_key(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];
	static const ioremap::elliptics::key key{"test_write_plain_into_nonexistent_key's key"};

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	auto write_and_check = [&] (const std::string &json, const std::string &data) {
		auto async = s.write_plain(key, json, data, 0);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), -ENOENT);
	};

	write_and_check("", "some data");
	write_and_check("{\"some\":\"json\"}", "");
	write_and_check("{\"some\":\"json\"}", "some data");
}

void test_write_plain_into_committed_key(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];
	static const ioremap::elliptics::key key{"test_write_plain_into_committed_key's key"};

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	{
		auto async = s.write(key,
		                     "{\"json\":\"isn't matter\"}", 100,
		                     "data isn't matter too", 100);

		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), 0);
	}

	auto write_and_check = [&] (const std::string &json, const std::string &data) {
		auto async = s.write_plain(key, json, data, 0);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), -EPERM);
	};

	write_and_check("", "some data");
	write_and_check("{\"some\":\"json\"}", "");
	write_and_check("{\"some\":\"json\"}", "some data");
}

void test_write_cas(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	static const ioremap::elliptics::key key{"test_write_cas's key"};
	static const std::string json{"{\"some_field\":\"some_field's data\"}"};
	uint64_t json_capacity = 100;
	static const std::string data{"test_write_cas's data"};
	uint64_t data_capacity = 200;
	constexpr dnet_time old_timestamp{1, 2};
	constexpr dnet_time cur_timestamp{3, 4};
	constexpr dnet_time new_timestamp{5, 6};

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_ioflags(s.get_ioflags() | DNET_IO_FLAGS_CAS_TIMESTAMP);

	auto write = [&] (const int expected_status) {
		auto async = s.write(key, json, json_capacity, data, data_capacity);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), expected_status);
	};

	s.set_timestamp(cur_timestamp);
	s.set_json_timestamp(cur_timestamp);
	write(0);

	s.set_timestamp(old_timestamp);
	s.set_json_timestamp(cur_timestamp);
	write(-EBADFD);

	s.set_timestamp(cur_timestamp);
	s.set_json_timestamp(old_timestamp);
	write(-EBADFD);

	s.set_timestamp(new_timestamp);
	s.set_json_timestamp(cur_timestamp);
	write(0);

	s.set_timestamp(cur_timestamp);
	s.set_json_timestamp(new_timestamp);
	write(-EBADFD);

	s.set_timestamp(new_timestamp);
	s.set_json_timestamp(new_timestamp);
	write(0);

	// TODO: tests for write_prepare, write_plain, write_commit
}

void test_write_to_readonly_backend(const ioremap::elliptics::newapi::session &session, const nodes_data *setup) {
	auto &server = setup->nodes.front();
	const auto remote = server.remote();
	const auto &backend = server.config().backends.front();
	const auto backend_id = std::stoi(backend.string_value("backend_id"));

	auto s = session.clone();
	s.set_direct_id(remote, backend_id);
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	{
		auto async = s.make_readonly(remote, backend_id);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), 0);
	}

	{
		static const std::string key = "test_write_to_readonly_backend's key";
		static const std::string json = "{\"some_field\":\"some_field's data\"}";
		static const std::string data = "no matter data";
		s.set_groups({-1});
		auto async = s.write(key, json, 100, data, 100);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), -EROFS);
	}

	{
		auto async = s.make_writable(remote, backend_id);
		BOOST_REQUIRE_EQUAL(async.get().size(), 1);

		auto result = async.get()[0];
		BOOST_REQUIRE_EQUAL(result.status(), 0);
	}
}

void test_remove(const ioremap::elliptics::newapi::session &session, const dnet_time &sess_timestamp,
		 const record &record, int expected_status) {
	auto s = session.clone();
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_groups(groups);
	s.set_timestamp(sess_timestamp);

	auto async = s.remove(record.key);

	size_t count = 0;
	for (const auto &result: async) {
		BOOST_REQUIRE_EQUAL(result.status(), expected_status);
		++count;
	}
	BOOST_REQUIRE_EQUAL(count, groups.size());
}

void test_remove_corrupted(const ioremap::elliptics::newapi::session &session) {
	static const auto group = groups[0];

	auto s = session.clone();
	s.set_groups({group});
	s.set_exceptions_policy(ioremap::elliptics::session::no_exceptions);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);

	static const std::string key{"test_remove_corrupted key"};
	static const std::string data{"remove_corrupted data"};
	static const std::string json{R"json(
	{
		"key": "remove_corrupted json key"
	}
	)json"};
	write_and_corrupt_json(s, key, json, 0, data, 0);

	auto async = s.remove(key);
	BOOST_REQUIRE_EQUAL(async.get().size(), 1);

	auto result = async.get()[0];
	BOOST_REQUIRE_EQUAL(result.status(), 0);
}

void test_bulk_read(const ioremap::elliptics::newapi::session &session) {
	record record{
		std::string{"key"},
		0xff1ff2ff3,
		dnet_time{10, 20},
		dnet_time{10, 20},
		std::string{"{\"key\": \"key\"}"},
		512,
		std::string{"key data"},
		1024,
		(session.get_ioflags() & DNET_IO_FLAGS_CACHE) != 0
	};

	std::map<dnet_id, tests::record> records;
	std::set<dnet_id> responses;

	auto check_all_responses_presence = [&records, &responses] () {
		for (const auto &pair : records) {
			auto &key = pair.first;
			BOOST_REQUIRE(responses.find(key) != responses.end());
		}
	};

	std::vector<int> groups{1, 2, 3, 4, 5, 6};

	/*
	 * Step 1. Prepare test data: write keys to multiple groups. Each key will be presented
	 * exactly once in every group.
	 */
	const static size_t NUM_KEYS_IN_GROUP = 100;
	const static size_t NUM_KEYS_TOTAL = NUM_KEYS_IN_GROUP * groups.size();

	std::vector<std::tuple<ioremap::elliptics::newapi::async_write_result,
			       decltype(records)::const_iterator>> write_results;
	write_results.reserve(NUM_KEYS_TOTAL);

	std::vector<dnet_id> ids;
	ids.reserve(NUM_KEYS_TOTAL);

	auto s = session.clone();
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_trace_id(rand());
	s.set_user_flags(record.user_flags);
	s.set_timestamp(record.timestamp);
	s.set_json_timestamp(record.json_timestamp);

	for (size_t i = 0; i < NUM_KEYS_IN_GROUP; ++i) {
		for (const int group_id : groups) {
			record.key = key("bulk_key_" + std::to_string(i));
			record.key.transform(s);
			record.key.set_group_id(group_id);

			auto unique_suffix = std::to_string(group_id * NUM_KEYS_IN_GROUP + i);
			record.json = "{\"key\": \"bulk_json_" + unique_suffix + "\"}";
			record.data = "bulk_data_" + unique_suffix;

			s.set_groups({group_id});
			auto async = s.write(record.key,
					     record.json, record.json_capacity,
					     record.data, record.data_capacity);

			auto it = records.emplace(record.key.id(), record).first;

			write_results.emplace_back(std::move(async), it);
			ids.emplace_back(record.key.id());
		}
	}

	for (auto &r : write_results) {
		auto &async = std::get<0>(r);
		const auto &record = std::get<1>(r)->second;
		check_lookup_result(async, DNET_CMD_WRITE_NEW, record, 1);
	}

	/*
	 * Step 2. Check bulk read in normal conditions: read keys from all groups
	 * and check its data. Check it against different read methods:
	 * bulk_read, bulk_read_data, bulk_read_json.
	 */
	size_t count = 0;
	auto check_read_result = [&] (ioremap::elliptics::newapi::async_read_result &async,
				      bool check_json, bool check_data) {
		responses.clear();
		size_t count = 0;
		for (const auto &result: async) {
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_BULK_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			const auto cmd = result.command();
			responses.emplace(cmd->id);

			auto it = records.find(cmd->id);
			BOOST_REQUIRE(it != records.end());
			auto ref = it->second;

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &ref.json_timestamp), 0);
			BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			BOOST_REQUIRE_EQUAL(record_info.json_size, ref.json.size());
			if (!ref.in_cache) {
				BOOST_REQUIRE_EQUAL(record_info.json_capacity, ref.json_capacity);
			}

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &ref.timestamp), 0);
			BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(record_info.data_size, ref.data.size());

			if (check_json) {
				BOOST_REQUIRE_EQUAL(result.json().to_string(), ref.json);
			} else {
				BOOST_REQUIRE(result.json().to_string().empty());
			}

			if (check_data) {
				BOOST_REQUIRE_EQUAL(result.data().to_string(), ref.data);
			} else {
				BOOST_REQUIRE(result.data().to_string().empty());
			}

			++count;
		}
		BOOST_REQUIRE_EQUAL(count, NUM_KEYS_TOTAL);
		check_all_responses_presence();
	};

	auto async = s.bulk_read(ids);
	check_read_result(async, true, true);

	async = s.bulk_read_json(ids);
	check_read_result(async, true, false);

	async = s.bulk_read_data(ids);
	check_read_result(async, false, true);

	/*
	 * Step 3. Check bulk read from non-existent groups. Half of the keys will point to an
	 * invalid group. Read from invalid groups must raise ENXIO error for each corresponding key.
	 */
	std::vector<dnet_id> ids_invalid_group(ids);
	const int invalid_group = 42;
	for (size_t i = 0; i < ids_invalid_group.size(); ++i) {
		if (i % 2 == 0) {
			ids_invalid_group[i].group_id = invalid_group;
		}
	}

	async = s.bulk_read(ids_invalid_group);

	count = 0;
	for (const auto &result: async) {
		const auto cmd = result.command();

		BOOST_REQUIRE_EQUAL(cmd->cmd, DNET_CMD_BULK_READ_NEW);

		if (cmd->id.group_id == invalid_group) {
			BOOST_REQUIRE_EQUAL(result.status(), -ENXIO);
			BOOST_REQUIRE_EQUAL(result.error().code(), -ENXIO);
		} else {
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		}

		++count;
	}
	BOOST_REQUIRE_EQUAL(count, NUM_KEYS_TOTAL);

	/*
	 * Step 4. Check bulk read from normal groups, corrupted groups and groups without a key.
	 * At first, remove and corrupt all keys on appropriate groups. Then check that bulk read
	 * will raise ENOENT, EILSEQ errors for keys from empty and corrupted groups respectively.
	 * At the same time, read from normal groups must return valid data.
	 */
	auto group_iter = groups.begin();
	const int remove_group = *group_iter++;
	const int corrupt_group = *group_iter++;
	const int delay_group = *group_iter++;

	for (const auto &pair : records) {
		auto &key = pair.first;
		auto &record = pair.second;
		const int group_id = key.group_id;

		if (group_id == remove_group) {
			s.set_groups({group_id});
			s.remove(key);
		}

		if (group_id == corrupt_group) {
			s.set_groups({group_id});
			write_and_corrupt_data(s, record.key,
					       record.json, record.json_capacity,
					       record.data, record.data_capacity);
		}
	}

	async = s.bulk_read(ids);

	responses.clear();
	count = 0;
	for (const auto &result: async) {
		const auto cmd = result.command();

		BOOST_REQUIRE_EQUAL(cmd->cmd, DNET_CMD_BULK_READ_NEW);

		if (cmd->id.group_id == static_cast<uint32_t>(remove_group)) {
			BOOST_REQUIRE_EQUAL(result.status(), -ENOENT);
			BOOST_REQUIRE_EQUAL(result.error().code(), -ENOENT);
		} else if (cmd->id.group_id == static_cast<uint32_t>(corrupt_group)) {
			BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
			BOOST_REQUIRE_EQUAL(result.error().code(), -EILSEQ);
		} else {
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		}

		responses.emplace(cmd->id);

		auto it = records.find(cmd->id);
		BOOST_REQUIRE(it != records.end());

		++count;
	}
	BOOST_REQUIRE_EQUAL(count, NUM_KEYS_TOTAL);
	check_all_responses_presence();

	/*
	 * Step 5. Check that bulk read from slow group will raise ETIMEDOUT error.
	 */
	s.set_groups(groups);
	set_delay_for_groups(s, {delay_group}, (s.get_timeout() + 1) * 1000);

	async = s.bulk_read(ids);

	responses.clear();
	count = 0;
	for (const auto &result: async) {
		const auto cmd = result.command();

		BOOST_REQUIRE_EQUAL(cmd->cmd, DNET_CMD_BULK_READ_NEW);

		if (cmd->id.group_id == static_cast<uint32_t>(remove_group)) {
			BOOST_REQUIRE_EQUAL(result.status(), -ENOENT);
			BOOST_REQUIRE_EQUAL(result.error().code(), -ENOENT);
		} else if (cmd->id.group_id == static_cast<uint32_t>(corrupt_group)) {
			BOOST_REQUIRE_EQUAL(result.status(), -EILSEQ);
			BOOST_REQUIRE_EQUAL(result.error().code(), -EILSEQ);
		} else if (cmd->id.group_id == static_cast<uint32_t>(delay_group)) {
			BOOST_REQUIRE_EQUAL(result.status(), -ETIMEDOUT);
			BOOST_REQUIRE_EQUAL(result.error().code(), -ETIMEDOUT);
		} else {
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
		}

		responses.emplace(cmd->id);

		auto it = records.find(cmd->id);
		BOOST_REQUIRE(it != records.end());

		++count;
	}
	BOOST_REQUIRE_EQUAL(count, NUM_KEYS_TOTAL);
	check_all_responses_presence();

	set_delay_for_groups(s, {delay_group}, 0);
}

bool register_tests(const nodes_data *setup) {
	record record{
		std::string{"key"},
		0xff1ff2ff3,
		dnet_time{10, 20},
		dnet_time{10, 20},
		std::string{"{\"key\": \"key\"}"},
		512,
		std::string{"key data"},
		1024,
		false
	};
	uint32_t ioflags = 0;

	auto cache_record = record;
	cache_record.key = std::string{"cache_key"};
	cache_record.in_cache = true;

	auto n = setup->node->get_native();

	auto run_tests = [&] (bool in_cache) {
		if (in_cache) {
			record = cache_record;
			ioflags = DNET_IO_FLAGS_CACHE;
		}

		ELLIPTICS_TEST_CASE(test_write, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_lookup, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_json, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 0, 0);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 0, 1);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 0, std::numeric_limits<uint64_t>::max());
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 1, 0);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 2, 1);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 3, std::numeric_limits<uint64_t>::max());
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 0, 0);
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 0, 1);
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 0, std::numeric_limits<uint64_t>::max());
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 1, 0);
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 2, 1);
		ELLIPTICS_TEST_CASE(test_read, use_session(n, {}, 0, ioflags), record, 3, std::numeric_limits<uint64_t>::max());

		if (!in_cache) {
			ELLIPTICS_TEST_CASE(test_bulk_read, use_session(n, {}, 0, ioflags));
		}

		record.json = R"json({
			"record": {
				"key": "key",
				"useful": "some useful info about the key"}
		})json";
		record.json_timestamp = dnet_time{11,22};
		ELLIPTICS_TEST_CASE(test_update_json, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_json, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 0, 0);

		record.json = "";
		record.json_timestamp = dnet_time{12,23};
		ELLIPTICS_TEST_CASE(test_update_json, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_json, use_session(n, {}, 0, ioflags), record);
		ELLIPTICS_TEST_CASE(test_read_data, use_session(n, {}, 0, ioflags), record, 0, 0);

		if (!in_cache) {
			ELLIPTICS_TEST_CASE(test_update_bigger_json, use_session(n, {}, 0, ioflags), record);
		}

		record.key = {in_cache ? "cache_chunked_key" : "chunked_key"};
		record.json_timestamp = record.timestamp;
		ELLIPTICS_TEST_CASE(test_write_chunked, use_session(n, {}, 0, ioflags), record);

		if (!in_cache) {
			ELLIPTICS_TEST_CASE(test_update_json_noexist, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_update_json_uncommitted, use_session(n, {}, 0, ioflags));
		}

		ELLIPTICS_TEST_CASE(test_old_write_new_read_compatibility, use_session(n, {}, 0, ioflags));
		ELLIPTICS_TEST_CASE(test_new_write_old_read_compatibility, use_session(n, {}, 0, ioflags));

		if (!in_cache) {
			ELLIPTICS_TEST_CASE(test_read_corrupted_json, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_json_with_corrupted_data_part, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_json_with_big_capacity_and_corrupted_data_part, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_data_with_corrupted_json, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_data_with_corrupted_json_with_big_capacity, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_data_with_corrupted_data, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_data_part_with_corrupted_first_data, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_read_data_part_with_corrupted_second_data, use_session(n, {}, 0, ioflags));
		}

		ELLIPTICS_TEST_CASE(test_data_and_json_timestamp, use_session(n, {}, 0, ioflags));

		if (!in_cache) {
			ELLIPTICS_TEST_CASE(test_write_plain_into_nonexistent_key, use_session(n, {}, 0, ioflags));
			ELLIPTICS_TEST_CASE(test_write_plain_into_committed_key, use_session(n, {}, 0, ioflags));
		}

		ELLIPTICS_TEST_CASE(test_write_cas, use_session(n, {}, 0, ioflags));

		ELLIPTICS_TEST_CASE(test_write_to_readonly_backend, use_session(n, {}, 0, ioflags), setup);

		// Test key remove with and without CAS timestamp
		record.key = {in_cache ? "cache_remove_key" : "remove_key"};
		for (uint32_t flags : {DNET_IO_FLAGS_CAS_TIMESTAMP, 0}) {
			dnet_current_time(&record.timestamp);
			ELLIPTICS_TEST_CASE(test_write, use_session(n, {}, 0, ioflags), record);

			// if test uses CAS timestamp, then key removal with older session timestamp
			// must return -EBADFD
			auto sess_timestamp = record.timestamp;
			sess_timestamp.tsec -= 1;
			int expected_status = (flags == 0) ? 0 : -EBADFD;
			ELLIPTICS_TEST_CASE(test_remove, use_session(n, {}, 0, ioflags | flags),
					    sess_timestamp, record, expected_status);

			sess_timestamp = record.timestamp;
			expected_status = (flags == 0) ? -ENOENT : 0;
			ELLIPTICS_TEST_CASE(test_remove, use_session(n, {}, 0, ioflags | flags),
					    sess_timestamp, record, expected_status);

			if (!in_cache) {
				ELLIPTICS_TEST_CASE(test_remove_corrupted, use_session(n, {}, 0, ioflags | flags));
			}
		}
	};

	run_tests(false);
	run_tests(true);

	return true;
}

} // namespace all

namespace all_with_ack_filter {

using namespace tests;

record record{
	std::string{"test_write_with_all_with_ack_filter::key"},
	0xf1235f12431,
	dnet_time{100, 40},
	dnet_time{100, 40},
	std::string{"{\"key\":\"test_write_with_all_with_ack_filter::key\"}"},
	100,
	std::string{"test_write_with_all_with_ack_filter::data"},
	100,
	false};

void test_write(ioremap::elliptics::newapi::session &s) {
	s.set_groups(groups);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_user_flags(record.user_flags);
	s.set_timestamp(record.timestamp);

	auto async = s.write(record.key,
	                     record.json, record.json_capacity,
	                     record.data, record.data_capacity);

	check_lookup_result(async, DNET_CMD_WRITE_NEW, record, groups.size());
}

void test_lookup(ioremap::elliptics::newapi::session &s) {
	s.set_groups(groups);
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_user_flags(record.user_flags);
	s.set_timestamp(record.timestamp);

	auto async = s.lookup(record.key);

	check_lookup_result(async, DNET_CMD_LOOKUP_NEW, record, 1);
}

void test_read(ioremap::elliptics::newapi::session &s) {
	s.set_filter(ioremap::elliptics::filters::all_with_ack);
	s.set_user_flags(record.user_flags);
	s.set_timestamp(record.timestamp);

	size_t count = 0;
	for (const auto &group : groups) {
		s.set_groups({group});
		auto async = s.read(record.key, 0, 0);

		for (const auto &result: async) {
			(void)result.address();
			BOOST_REQUIRE_EQUAL(result.status(), 0);
			BOOST_REQUIRE_EQUAL(result.command()->cmd, DNET_CMD_READ_NEW);
			BOOST_REQUIRE_EQUAL(result.error().code(), 0);
			BOOST_REQUIRE_EQUAL(result.error().message(), "");

			auto record_info = result.record_info();

			BOOST_REQUIRE_EQUAL(record_info.user_flags, record.user_flags);
			BOOST_REQUIRE_EQUAL(record_info.record_flags,
			                    DNET_RECORD_FLAGS_EXTHDR | DNET_RECORD_FLAGS_CHUNKED_CSUM);

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.json_timestamp, &record.json_timestamp), 0);
			BOOST_REQUIRE_EQUAL(record_info.json_offset, 0);
			BOOST_REQUIRE_EQUAL(record_info.json_size, record.json.size());
			BOOST_REQUIRE_EQUAL(record_info.json_capacity, record.json_capacity);

			BOOST_REQUIRE_EQUAL(dnet_time_cmp(&record_info.data_timestamp, &record.timestamp), 0);
			BOOST_REQUIRE_EQUAL(record_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(record_info.data_size, record.data.size());

			BOOST_REQUIRE_EQUAL(result.json().to_string(), record.json);
			BOOST_REQUIRE_EQUAL(result.data().to_string(), record.data);

			auto io_info = result.io_info();

			BOOST_REQUIRE_EQUAL(io_info.json_size, record.json.size());
			BOOST_REQUIRE_EQUAL(io_info.data_offset, 0);
			BOOST_REQUIRE_EQUAL(io_info.data_size, record.data.size());

			++count;
		}
	}

	BOOST_REQUIRE_EQUAL(count, groups.size());
}

bool register_tests(const nodes_data *setup) {
	ioremap::elliptics::newapi::session s(*setup->node);

	auto n = setup->node->get_native();

	ELLIPTICS_TEST_CASE(test_write, use_session(n));
	ELLIPTICS_TEST_CASE(test_lookup, use_session(n));
	ELLIPTICS_TEST_CASE(test_read, use_session(n));

	return true;
}

} /* namespace test_all_with_ack_filter */

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

	return tests::configure_test_setup(path);
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

bool init_func()
{
	return all::register_tests(setup.get())
		&& all_with_ack_filter::register_tests(setup.get())
		;
}

}

int main(int argc, char *argv[])
{
	srand(time(nullptr));

	// we own our test setup
	setup = configure_test_setup_from_args(argc, argv);

	int result = unit_test_main(init_func, argc, argv);

	// disassemble setup explicitly, to be sure about where its lifetime ends
	setup.reset();

	return result;
}
