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

#include <boost/algorithm/string/split.hpp>

#include "ProtocolStrategy.h"
#include <common/log.h>
#include "protocol_strategy.h"

namespace caspar { namespace IO {

/**
 * A protocol strategy factory adapter for converting incoming data from a
 * specific codepage to utf-16. The client_connection will do the reversed
 * conversion.
 *
 * The adapter is not safe if the codepage contains multibyte-characters and
 * the data is chunked with potentially incomplete characters, therefore it
 * must be wrapped in an adapter providing complete chunks like
 * delimiter_based_chunking_strategy_factory.
 */
class to_unicode_adapter_factory : public protocol_strategy_factory<char>
{
    std::string                             codepage_;
    protocol_strategy_factory<wchar_t>::ptr unicode_strategy_factory_;

  public:
    to_unicode_adapter_factory(const std::string&                             codepage,
                               const protocol_strategy_factory<wchar_t>::ptr& unicode_strategy_factory);

    protocol_strategy<char>::ptr create(const client_connection<char>::ptr& client_connection) override;
};

/**
 * Protocol strategy adapter for ensuring that only complete chunks or
 * "packets" are delivered to the wrapped strategy. The chunks are determined
 * by a given delimiter.
 */
template <class CharT>
class delimiter_based_chunking_strategy : public protocol_strategy<CharT>
{
    /// The longest line held while waiting for its delimiter. Uncapped, `input_ += data` grew for
    /// as long as a peer sent bytes without a CRLF, and every read rescanned the whole buffer. A
    /// line past the cap is DISCARDED up to its delimiter, so its tail is not parsed as a command.
    static constexpr std::size_t max_line = 64u * 1024u * 1024u;

    std::basic_string<CharT>               delimiter_;
    std::basic_string<CharT>               input_;
    std::size_t                            scanned_    = 0;     //< input_ before this holds no delimiter
    bool                                   discarding_ = false; //< inside an over-long line
    typename protocol_strategy<CharT>::ptr strategy_;

  public:
    delimiter_based_chunking_strategy(const std::basic_string<CharT>&               delimiter,
                                      const typename protocol_strategy<CharT>::ptr& strategy)
        : delimiter_(delimiter)
        , strategy_(strategy)
    {
    }

    // THE WHOLE DELIMITER, EVERY TIME, and the line CONSUMED BEFORE IT IS PARSED (from CasparVP).
    //
    // The first delimiter of a read was found with `find` and every later one with
    // `find_first_of`, which matches EITHER character. So a read ending between the CR and the LF
    // of a line that was not the first of that read parsed and EXECUTED the line, then threw
    // out_of_range from `substr` before `input_` was reassigned -- and the next read completed the
    // CRLF and the same line ran a SECOND time. Measured with tools/test/live_input_test.py:
    // "VERSION<CR><LF>VERSION<CR>" then "<LF>" gave three replies. A lone CR or LF inside a
    // later line
    // also split it there.
    void parse(const std::basic_string<CharT>& data) override
    {
        input_ += data;

        for (;;) {
            const auto delim_pos = input_.find(delimiter_, scanned_);
            if (delim_pos == std::basic_string<CharT>::npos) {
                // Rescan only where a delimiter could still begin: its first character may be the
                // last one received.
                const auto keep = delimiter_.size() > 0 ? delimiter_.size() - 1 : 0;
                scanned_        = input_.size() > keep ? input_.size() - keep : 0;
                if (input_.size() > max_line) {
                    if (!discarding_)
                        CASPAR_LOG(error) << L"[amcp] a line exceeded " << max_line
                                          << L" characters without a delimiter; discarding it";
                    input_.erase(0, scanned_);
                    scanned_    = 0;
                    discarding_ = true;
                }
                return;
            }

            auto line = input_.substr(0, delim_pos);
            input_.erase(0, delim_pos + delimiter_.size());
            scanned_ = 0;

            if (discarding_) {
                // The end of the over-long line: drop it, and resume with what follows.
                discarding_ = false;
                continue;
            }

            try {
                strategy_->parse(line);
            } catch (const std::exception& e) {
                // One bad line must not strand the complete lines behind it in `input_`.
                CASPAR_LOG(error) << L"[amcp] a line could not be parsed: " << e.what();
            }
        }
    }
};

template <class CharT>
class delimiter_based_chunking_strategy_factory : public protocol_strategy_factory<CharT>
{
    std::basic_string<CharT>                       delimiter_;
    typename protocol_strategy_factory<CharT>::ptr strategy_factory_;

  public:
    delimiter_based_chunking_strategy_factory(const std::basic_string<CharT>&                       delimiter,
                                              const typename protocol_strategy_factory<CharT>::ptr& strategy_factory)
        : delimiter_(delimiter)
        , strategy_factory_(strategy_factory)
    {
    }

    typename protocol_strategy<CharT>::ptr
    create(const typename client_connection<CharT>::ptr& client_connection) override
    {
        return spl::make_shared<delimiter_based_chunking_strategy<CharT>>(delimiter_,
                                                                          strategy_factory_->create(client_connection));
    }
};

}} // namespace caspar::IO
