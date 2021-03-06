//
// PROJECT:         Aspia Remote Desktop
// FILE:            protocol/io_queue.cc
// LICENSE:         Mozilla Public License Version 2.0
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "protocol/io_queue.h"

namespace aspia {

IOQueue::IOQueue(ProcessMessageCallback process_message_callback) :
    process_message_callback_(std::move(process_message_callback)),
    event_(WaitableEvent::ResetPolicy::AUTOMATIC,
           WaitableEvent::InitialState::NOT_SIGNALED)
{
    Start();
}

IOQueue::~IOQueue()
{
    StopSoon();
    event_.Signal();
    Join();
}

void IOQueue::Add(IOBuffer buffer)
{
    if (IsStopping())
        return;

    {
        std::lock_guard<std::mutex> lock(queue_lock_);
        queue_.push(std::move(buffer));
    }

    event_.Signal();
}

void IOQueue::Run()
{
    for (;;)
    {
        for (;;)
        {
            bool is_empty;

            {
                std::lock_guard<std::mutex> lock(queue_lock_);
                is_empty = queue_.empty();
            }

            if (is_empty)
                break;

            IOBuffer buffer;

            {
                std::lock_guard<std::mutex> lock(queue_lock_);
                buffer = std::move(queue_.front());
                queue_.pop();
            }

            process_message_callback_(buffer);
        }

        if (IsStopping())
            return;

        event_.Wait();
    }
}

} // namespace aspia
