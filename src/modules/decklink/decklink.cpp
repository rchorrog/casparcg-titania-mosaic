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
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "StdAfx.h"

#include "decklink.h"
#include "util/util.h"

#include "consumer/decklink_consumer.h"
#include "producer/decklink_producer.h"

#include <core/consumer/frame_consumer.h>
#include <core/producer/frame_producer.h>

#include <protocol/amcp/amcp_command_context.h>
#include <protocol/amcp/amcp_command_repository_wrapper.h>

#include <sstream>

#include "decklink_api.h"

namespace caspar { namespace decklink {

std::wstring get_version()
{
    std::wstring ver = L"Not found";

    struct co_init init;

    try {
        ver = decklink::version(create_iterator());
    } catch (...) {
    }

    return ver;
}

struct device_info
{
    int          index; // 1-based: el valor que acepta "PLAY ... DECKLINK DEVICE <n>"
    std::wstring model;
    int64_t      persistent_id;
};

// Lanza si no se puede crear el iterador (p. ej. Desktop Video no instalado); sin tarjetas devuelve vacio.
std::vector<device_info> list_devices()
{
    std::vector<device_info> devices;

    struct co_init init;

    auto       pDecklinkIterator = create_iterator();
    IDeckLink* decklink;
    for (int n = 1; pDecklinkIterator->Next(&decklink) == S_OK; ++n) {
        auto decklink_com = wrap_raw<com_ptr>(decklink);
        auto attributes   = iface_cast<IDeckLinkProfileAttributes>(decklink_com);

        int64_t id = 0;
        attributes->GetInt(BMDDeckLinkPersistentID, &id);

        devices.push_back({n, get_model_name(decklink), id});
        decklink->Release();
    }

    return devices;
}

std::vector<std::wstring> device_list()
{
    std::vector<std::wstring> devices;

    try {
        for (const auto& device : list_devices()) {
            devices.push_back(device.model + L" [" + std::to_wstring(device.index) + L"] (" +
                              std::to_wstring(device.persistent_id) + L")");
        }
    } catch (...) {
    }

    return devices;
}

// DECKLINK LIST: una linea por tarjeta, '<indice> "<modelo>" <persistentId>', como NDI LIST. Sin tarjetas, solo la
// cabecera y la linea vacia final; si el API de DeckLink no esta disponible, 501.
std::wstring list_command(protocol::amcp::command_context& ctx)
{
    try {
        std::wstringstream reply;
        reply << L"200 DECKLINK LIST OK\r\n";
        for (const auto& device : list_devices()) {
            std::wstring model = device.model;
            for (auto& c : model) {
                if (c == L'"') {
                    c = L'\'';
                }
            }
            reply << device.index << L" \"" << model << L"\" " << device.persistent_id << L"\r\n";
        }
        reply << L"\r\n";
        return reply.str();
    } catch (...) {
        return L"501 DECKLINK LIST FAILED\r\n";
    }
}

void init(const core::module_dependencies& dependencies)
{
    dependencies.consumer_registry->register_consumer_factory(L"Decklink Consumer", create_consumer);
    dependencies.consumer_registry->register_preconfigured_consumer_factory(L"decklink", create_preconfigured_consumer);
    dependencies.producer_registry->register_producer_factory(L"Decklink Producer", create_producer);
    dependencies.command_repository->register_command(L"Query Commands", L"DECKLINK LIST", list_command, 0);

    auto devices = device_list();
    if (!devices.empty()) {
        CASPAR_LOG(info) << L"Decklink devices found:";
        for (const auto& device : devices) {
            CASPAR_LOG(info) << L" - " << device;
        }
    }
}

}} // namespace caspar::decklink
