/*************************************************************************
 * This file is part of tuna
 * github.com/univrsal/tuna
 * Copyright 2020 univrsal <uni@vrsal.cf>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "tuna_thread.hpp"
#include "../gui/music_control.hpp"
#include "../gui/tuna_gui.hpp"
#include "../query/music_source.hpp"
#include "config.hpp"
#include "utility.hpp"
#include <algorithm>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

namespace tuna_thread {
volatile bool thread_flag = false;
song copy;
std::mutex thread_mutex;
std::mutex copy_mutex;
std::thread thread_handle;

bool start()
{
    std::lock_guard<std::mutex> lock(thread_mutex);
    if (thread_flag)
        return true;
    thread_flag = true;
    thread_handle = std::thread(thread_method);
    bool result = thread_handle.native_handle();
    thread_flag = result;
    return result;
}

void stop()
{
    if (!thread_flag)
        return;
    {
        std::lock_guard<std::mutex> lock(thread_mutex);
        /* Set status to noting before stopping */
        auto src = music_sources::selected_source_unsafe();
        src->reset_info();
        util::handle_outputs(src->song_info());
        thread_flag = false;
    }
    thread_handle.join();
    util::reset_cover();
}

void thread_method()
{
    os_set_thread_name("tuna-query");

    while (true) {
        const uint64_t time = os_gettime_ns() / 1000000;
        {
            std::lock_guard<std::mutex> lock(thread_mutex);
            auto ref = music_sources::selected_source_unsafe();
            if (ref) {

                ref->refresh();
                auto s = ref->song_info();

                /* Make a copy for the progress bar source, because it can't
                 * wait for the other processes to finish, otherwise it'll block
                 * the video thread
                 */
                copy_mutex.lock();
                copy = s;
                copy_mutex.unlock();

                /* Process song data */
                util::handle_outputs(s);
                if (config::download_cover)
                    ref->handle_cover();
                if (!thread_flag)
                    break;
            }
        }

        /* Calculate how long refresh took and only wait the remaining time */
        uint64_t delta = std::min<uint64_t>((os_gettime_ns() / 1000000) - time, config::refresh_rate);
        uint64_t wait = config::refresh_rate - delta;
        os_sleep_ms(wait);
    }
    binfo("Query thread stopped.");
}
}
