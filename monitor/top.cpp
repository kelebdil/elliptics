/*
 * Copyright 2015+ Budnik Andrey <budnik27@gmail.com>
 *
 * This file is part of Elliptics.
 *
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "top.hpp"

#include "elliptics/interface.h"

namespace ioremap { namespace monitor {

top_stats::top_stats(size_t top_length, size_t events_size, int period_in_seconds)
: m_stats(events_size, period_in_seconds)
, m_top_length(top_length)
, m_period_in_seconds(period_in_seconds) {}

void top_stats::update_stats(const struct dnet_cmd *cmd, uint64_t size)
{
	const bool is_read = (cmd->cmd == DNET_CMD_READ) || (cmd->cmd == DNET_CMD_READ_NEW);
	if (size > 0 && is_read) {
		key_stat_event event(cmd->id, size, 1., time(nullptr));
		m_stats.add_event(event, event.get_time());
	}
}

top_provider::top_provider(std::shared_ptr<top_stats> top_stats)
: m_top_stats(top_stats)
{
}

static void fill_top_stat(const key_stat_event &key_event,
                          rapidjson::Value &stat_array,
                          rapidjson::Document::AllocatorType &allocator) {
	rapidjson::Value key_stat(rapidjson::kObjectType);

	key_stat.AddMember("group", key_event.get_key()->group_id, allocator);
	rapidjson::Value id;
	id.SetString(dnet_dump_id_str_full(key_event.get_key()->id), allocator);
	key_stat.AddMember("id", id, allocator);
	key_stat.AddMember("size", key_event.get_weight(), allocator);
	key_stat.AddMember("frequency", static_cast<uint64_t>(key_event.get_frequency()), allocator);

	stat_array.PushBack(key_stat, allocator);
}

void top_provider::statistics(uint64_t categories,
                              rapidjson::Value &value,
                              rapidjson::Document::AllocatorType &allocator) const {
	if (!(categories & DNET_MONITOR_TOP))
		return;

	value.SetObject();

	std::vector<key_stat_event> top_size_keys;
	auto& event_stats = m_top_stats->get_stats();
	event_stats.get_top(m_top_stats->get_top_length(), time(nullptr), top_size_keys);

	rapidjson::Value stat_array(rapidjson::kArrayType);
	stat_array.Reserve(top_size_keys.size(), allocator);

	for (auto it = top_size_keys.cbegin(); it != top_size_keys.cend(); ++it) {
		const auto &key_stat = *it;
		fill_top_stat(key_stat, stat_array, allocator);
	}

	value.AddMember("top_result_limit", m_top_stats->get_top_length(), allocator);
	value.AddMember("period_in_seconds", m_top_stats->get_period(), allocator);
	value.AddMember("top_by_size", stat_array, allocator);
}

}} /* namespace ioremap::monitor */
