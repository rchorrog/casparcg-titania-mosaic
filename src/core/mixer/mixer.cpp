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

#include "../StdAfx.h"

#include "mixer.h"

#include "../frame/frame.h"

#include "audio/audio_mixer.h"
#include "image/image_mixer.h"

#include <common/diagnostics/graph.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace caspar { namespace core {

struct mixer::impl
{
    monitor::state                       state_;
    int                                  channel_index_;
    spl::shared_ptr<diagnostics::graph>  graph_;
    audio_mixer                          audio_mixer_{graph_};
    spl::shared_ptr<image_mixer>         image_mixer_;
    std::queue<std::future<const_frame>> buffer_;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

    impl(int channel_index, spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer)
        : channel_index_(channel_index)
        , graph_(std::move(graph))
        , image_mixer_(std::move(image_mixer))
    {
    }

    const_frame operator()(std::vector<draw_frame> frames, const std::vector<int>& layer_indices, const video_format_desc& format_desc, int nb_samples)
    {
        image_mixer_->update_aspect_ratio(static_cast<double>(format_desc.square_width) /
                                          static_cast<double>(format_desc.square_height));

        // ── Construir mapa tag→layer_index ANTES de mezclar ──────────────────
        // No usamos f.audio_data() para evitar el caso donde size_==0 en un
        // frame válido pero sin audio (ej. primera entrega, HTML overlay, etc.)
        struct tag_finder : public frame_visitor
        {
            const void* tag = nullptr;
            void push(const frame_transform&) override {}
            void pop() override {}
            void visit(const const_frame& f) override { if (!tag) tag = f.stream_tag(); }
        };
        std::map<const void*, int> tag_to_layer;
        for (size_t i = 0; i < frames.size() && i < layer_indices.size(); ++i) {
            tag_finder finder;
            frames[i].accept(finder);
            if (finder.tag) tag_to_layer[finder.tag] = layer_indices[i];
        }
        // ─────────────────────────────────────────────────────────────────────

        for (auto& frame : frames) {
            frame.accept(audio_mixer_);
            frame.transform().image_transform.layer_depth = 1;
            frame.accept(*image_mixer_);
        }

        auto result = image_mixer_->render(format_desc);
        auto audio  = audio_mixer_(format_desc, nb_samples);

        // Estado nuevo en cada frame: una layer que deja de tener audio (STOP, CLEAR, fuente sin audio) deja de
        // publicar su peak en vez de repetir el último valor medido.
        //
        // ── Per-layer audio peak → OSC /channel/N/mixer/layer/L/audio/peak/CH ──
        // Se reúnen todas las claves y se construye el flat_map de una vez, ordenado: insertarlas una a una
        // (layers en orden de puntero, canales 0,1,..,10 en orden numérico) desplazaba la tabla en cada
        // inserción. Los canales 0-7 (los que pinta el vúmetro) se publican siempre; los 8-15 solo si traen
        // señal — siempre a -100 eran la mitad de los mensajes OSC del canal. Sin peak_mono: osc-bridge.js lo
        // descarta y ninguna plantilla lo usa.
        {
            constexpr size_t osc_peak_channels_always = 8;

            std::vector<std::pair<std::string, monitor::vector_t>> entries;
            for (const auto& p : audio_mixer_.state()) {
                entries.emplace_back("audio/" + p.first, p.second);
            }

            const auto& tag_peaks = audio_mixer_.per_tag_peaks();
            for (const auto& kv : tag_to_layer) {
                auto it = tag_peaks.find(kv.first);
                if (it == tag_peaks.end()) continue;
                const auto&       ch_peaks = it->second;
                const std::string prefix   = "layer/" + std::to_string(kv.second) + "/audio/peak/";
                // Emitir peak individual por canal de audio (0=L, 1=R, ...)
                for (size_t ch = 0; ch < ch_peaks.size(); ++ch) {
                    float linear = ch_peaks[ch];
                    if (ch >= osc_peak_channels_always && linear <= 1e-6f) continue;
                    float db = linear > 1e-6f ? 20.0f * std::log10(linear) : -100.0f;
                    entries.emplace_back(prefix + std::to_string(ch), monitor::vector_t{monitor::data_t(db)});
                }
            }

            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            // Rango ya ordenado: el constructor de flat_map lo inserta en tiempo lineal (y descarta duplicados).
            state_ = monitor::state(monitor::data_map_t(entries.begin(), entries.end()));
        }
        // ─────────────────────────────────────────────────────────────────────

        auto depth = image_mixer_->depth();

        buffer_.push(std::async(
            std::launch::deferred,
            [result = std::move(result),
             audio  = std::move(audio),
             graph  = graph_,
             depth,
             format_desc,
             tag = this]() mutable {
                auto desc = pixel_format_desc(pixel_format::bgra);
                desc.planes.push_back(pixel_format_desc::plane(format_desc.width, format_desc.height, 4, depth));
                std::vector<array<const uint8_t>> image_data;
                auto                              tuple = std::move(result.get());
                image_data.emplace_back(std::move(std::get<0>(tuple)));
                return const_frame(tag, std::move(image_data), std::move(audio), desc, std::move(std::get<1>(tuple)));
            }));

        if (buffer_.size() <= format_desc.field_count) {
            return const_frame{};
        }

        auto frame = std::move(buffer_.front().get());
        buffer_.pop();
        return frame;
    }

    void set_master_volume(float volume) { audio_mixer_.set_master_volume(volume); }

    float get_master_volume() { return audio_mixer_.get_master_volume(); }
};

mixer::mixer(int channel_index, spl::shared_ptr<diagnostics::graph> graph, spl::shared_ptr<image_mixer> image_mixer)
    : impl_(new impl(channel_index, std::move(graph), std::move(image_mixer)))
{
}
void        mixer::set_master_volume(float volume) { impl_->set_master_volume(volume); }
float       mixer::get_master_volume() { return impl_->get_master_volume(); }
const_frame mixer::operator()(std::vector<draw_frame> frames, const std::vector<int>& layer_indices, const video_format_desc& format_desc, int nb_samples)
{
    return (*impl_)(std::move(frames), layer_indices, format_desc, nb_samples);
}
mutable_frame mixer::create_frame(const void* tag, const pixel_format_desc& desc)
{
    return impl_->image_mixer_->create_frame(tag, desc);
}
core::monitor::state mixer::state() const { return impl_->state_; }

common::bit_depth mixer::depth() const { return impl_->image_mixer_->depth(); }
}} // namespace caspar::core
