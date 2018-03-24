/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Helge Norberg, helge.norberg@svt.se
 */

#pragma once

#include "../util/ClientInfo.h"
#include "AMCPCommand.h"

#include <common/memory.h>

#include <core/help/help_repository.h>

#include <functional>

namespace caspar { namespace protocol { namespace amcp {

class amcp_command_repository : boost::noncopyable
{
  public:
    amcp_command_repository(const std::vector<channel_context>&      channels,
                            const spl::shared_ptr<core::help_repository>&                 help_repo);

    std::shared_ptr<AMCPCommand>
         parse_command(IO::ClientInfoPtr client, std::list<std::wstring> tokens, const std::wstring& request_id) const;
    bool check_channel_lock(IO::ClientInfoPtr client, int channel_index) const;

    const std::vector<channel_context>& channels() const;

    void register_command(std::wstring              category,
                          std::wstring              name,
                          core::help_item_describer describer,
                          amcp_command_func         command,
                          int                       min_num_params);
    void register_channel_command(std::wstring              category,
                                  std::wstring              name,
                                  core::help_item_describer describer,
                                  amcp_command_func         command,
                                  int                       min_num_params);
    spl::shared_ptr<core::help_repository> help_repo() const;

  private:
    struct impl;
    spl::shared_ptr<impl> impl_;
};

}}} // namespace caspar::protocol::amcp
