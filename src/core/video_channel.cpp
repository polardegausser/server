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

#include "video_channel.h"

#include "video_format.h"

#include "consumer/output.h"
#include "frame/draw_frame.h"
#include "frame/frame_transform.h"
#include "frame/frame.h"
#include "frame/frame_factory.h"
#include "mixer/mixer.h"
#include "producer/stage.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/timer.h>

#include <core/diagnostics/call_context.h>
#include <core/mixer/image/image_mixer.h>

#include <boost/lexical_cast.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace caspar { namespace core {

struct video_channel::impl final
{
    monitor::state state_;

    const int index_;

    mutable std::mutex      format_desc_mutex_;
    core::video_format_desc format_desc_;

    const spl::shared_ptr<caspar::diagnostics::graph> graph_ = [](int index) {
        core::diagnostics::scoped_call_context save;
        core::diagnostics::call_context::for_thread().video_channel = index;
        return spl::make_shared<caspar::diagnostics::graph>();
    }(index_);

    caspar::core::output         output_;
    spl::shared_ptr<image_mixer> image_mixer_;
    caspar::core::mixer          mixer_;
    caspar::core::stage          stage_;

    std::function<void(const monitor::state&)> tick_;

    std::map<int, std::weak_ptr<core::route>> routes_;
    std::mutex                                routes_mutex_;

    std::atomic<bool> abort_request_{false};
    std::thread       thread_;

  public:
    impl(int index, const core::video_format_desc& format_desc, std::unique_ptr<image_mixer> image_mixer, std::function<void(const monitor::state&)> tick)
        : index_(index)
        , format_desc_(format_desc)
        , output_(graph_, format_desc, index)
        , image_mixer_(std::move(image_mixer))
        , mixer_(index, graph_, image_mixer_)
        , stage_(index, graph_)
        , tick_(tick)
    {
        graph_->set_color("produce-time", caspar::diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("mix-time", caspar::diagnostics::color(1.0f, 0.0f, 0.9f, 0.8f));
        graph_->set_color("consume-time", caspar::diagnostics::color(1.0f, 0.4f, 0.0f, 0.8f));
        graph_->set_text(print());
        caspar::diagnostics::register_graph(graph_);

        CASPAR_LOG(info) << print() << " Successfully Initialized.";

        thread_ = std::thread([=] {
            while (!abort_request_) {
                try {
                    auto format_desc = video_format_desc();

                    state_.clear();

                    // Produce
                    caspar::timer produce_timer;
                    auto          stage_frames = stage_(format_desc);
                    graph_->set_value("produce-time", produce_timer.elapsed() * format_desc.fps * 0.5);

                    state_.insert_or_assign("stage", stage_.state());

                    {
                        std::vector<core::draw_frame> frames;

                        std::lock_guard<std::mutex> lock(routes_mutex_);

                        for (auto& p : stage_frames) {
                            frames.push_back(p.second);

                            auto it = routes_.find(p.first);
                            if (it != routes_.end()) {
                                auto route = it->second.lock();
                                if (route) {
                                    route->signal(draw_frame::pop(p.second));
                                }
                            }
                        }

                        auto it = routes_.find(-1);
                        if (it != routes_.end()) {
                            auto route = it->second.lock();
                            if (route) {
                                route->signal(core::draw_frame(std::move(frames)));
                            }
                        }
                    }

                    // Mix
                    caspar::timer mix_timer;
                    auto          mixed_frame = mixer_(std::move(stage_frames), format_desc);
                    graph_->set_value("mix-time", mix_timer.elapsed() * format_desc.fps * 0.5);

                    state_.insert_or_assign("mixer", mixer_.state());

                    // Consume
                    caspar::timer consume_timer;
                    output_(std::move(mixed_frame), format_desc).wait();
                    graph_->set_value("consume-time", consume_timer.elapsed() * format_desc.fps * 0.5);

                    state_.insert_or_assign("output", output_.state());

                    tick_(state_);
                } catch (...) {
                    CASPAR_LOG_CURRENT_EXCEPTION();
                }
            }
        });
    }

    ~impl()
    {
        CASPAR_LOG(info) << print() << " Uninitializing.";
        abort_request_ = true;
        thread_.join();
    }

    std::shared_ptr<core::route> route(int index = -1)
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);

        auto route = routes_[index].lock();
        if (!route) {
            route = std::make_shared<core::route>();
            route->format_desc = format_desc_;
            route->name = boost::lexical_cast<std::wstring>(index_);
            if (index != -1) {
                route->name += L"/" + boost::lexical_cast<std::wstring>(index);
            }
            routes_[index] = route;
        }

        return route;
    }

    core::video_format_desc video_format_desc() const
    {
        std::lock_guard<std::mutex> lock(format_desc_mutex_);
        return format_desc_;
    }

    void video_format_desc(const core::video_format_desc& format_desc)
    {
        std::lock_guard<std::mutex> lock(format_desc_mutex_);
        format_desc_ = format_desc;
        stage_.clear();
    }

    std::wstring print() const
    {
        return L"video_channel[" + boost::lexical_cast<std::wstring>(index_) + L"|" + video_format_desc().name + L"]";
    }

    int index() const { return index_; }
};

video_channel::video_channel(int                            index,
                             const core::video_format_desc& format_desc,
                             std::unique_ptr<image_mixer>   image_mixer,
                             std::function<void(const monitor::state&)> tick)
    : impl_(new impl(index, format_desc, std::move(image_mixer), tick))
{
}
video_channel::~video_channel() {}
const stage&                   video_channel::stage() const { return impl_->stage_; }
stage&                         video_channel::stage() { return impl_->stage_; }
const mixer&                   video_channel::mixer() const { return impl_->mixer_; }
mixer&                         video_channel::mixer() { return impl_->mixer_; }
const output&                  video_channel::output() const { return impl_->output_; }
output&                        video_channel::output() { return impl_->output_; }
spl::shared_ptr<frame_factory> video_channel::frame_factory() { return impl_->image_mixer_; }
core::video_format_desc        video_channel::video_format_desc() const { return impl_->video_format_desc(); }
void                           core::video_channel::video_format_desc(const core::video_format_desc& format_desc)
{
    impl_->video_format_desc(format_desc);
}
int               video_channel::index() const { return impl_->index(); }
const monitor::state& video_channel::state() const { return impl_->state_; }

std::shared_ptr<route> video_channel::route(int index) { return impl_->route(index); }

}} // namespace caspar::core
