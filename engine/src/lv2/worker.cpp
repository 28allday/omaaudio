#include "lv2/worker.h"

#include <cstring>

namespace omawave::lv2 {

Worker::Worker()
    : thread_([this] { loop(); })
{
}

Worker::~Worker()
{
    running_.store(false, std::memory_order_release);
    wake_.release();
    thread_.join();
}

LV2_Worker_Status Worker::schedule(WorkerClient* client, std::uint32_t size,
                                   const void* data) noexcept
{
    if (client == nullptr || (data == nullptr && size > 0))
        return LV2_WORKER_ERR_UNKNOWN;
    if (size > kMaxPayload)
        return LV2_WORKER_ERR_NO_SPACE;

    Record* record = requests_.beginPush();
    if (record == nullptr)
        return LV2_WORKER_ERR_NO_SPACE;

    record->client = client;
    record->size = size;
    if (size > 0)
        std::memcpy(record->data.data(), data, size);
    requests_.commitPush();

    wake_.release();
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Worker::respond(WorkerClient* client, std::uint32_t size,
                                  const void* data) noexcept
{
    if (client == nullptr || (data == nullptr && size > 0))
        return LV2_WORKER_ERR_UNKNOWN;
    if (size > kMaxPayload)
        return LV2_WORKER_ERR_NO_SPACE;

    Record* record = responses_.beginPush();
    if (record == nullptr)
        return LV2_WORKER_ERR_NO_SPACE;

    record->client = client;
    record->size = size;
    if (size > 0)
        std::memcpy(record->data.data(), data, size);
    responses_.commitPush();
    return LV2_WORKER_SUCCESS;
}

void Worker::deliverResponses() noexcept
{
    while (Record* record = responses_.peek()) {
        record->client->deliverResponse(record->size, record->data.data());
        responses_.pop();
    }
}

void Worker::loop()
{
    for (;;) {
        wake_.acquire();
        if (!running_.load(std::memory_order_acquire))
            return;
        // Drain whole batches: several schedules may share one wake, and a spurious
        // wake finds the ring empty and goes straight back to sleep.
        while (Record* record = requests_.peek()) {
            record->client->performWork(record->size, record->data.data());
            requests_.pop();
        }
    }
}

} // namespace omawave::lv2
