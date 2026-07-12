#include <agent/internal/sse_server_channel.hpp>

#include <cassert>
#include <chrono>
#include <string>

using agent_framework::internal::SseServerChannel;

int main() {
    SseServerChannel channel(2, 3);
    assert(channel.capacity() == 2);
    assert(channel.push_framed("one") == SseServerChannel::PushResult::accepted);
    assert(channel.push_framed("two") == SseServerChannel::PushResult::accepted);
    assert(channel.push_framed("three") == SseServerChannel::PushResult::dropped_oldest);
    assert(channel.pending_count() == 2);
    assert(channel.dropped_count() == 1);

    std::string out;
    assert(channel.pop_or_wait(out, std::chrono::milliseconds(1)) ==
           SseServerChannel::PopResult::chunk);
    assert(out == "two");
    assert(channel.push_framed("four") == SseServerChannel::PushResult::accepted);
    assert(channel.push_framed("five") == SseServerChannel::PushResult::dropped_oldest);
    assert(channel.push_framed("six") == SseServerChannel::PushResult::dropped_oldest);
    assert(channel.is_closed());
    assert(channel.dropped_count() == 3);
    assert(channel.push_framed("ignored") == SseServerChannel::PushResult::closed);

    while (channel.pop_or_wait(out, std::chrono::milliseconds(1)) ==
           SseServerChannel::PopResult::chunk) {
    }
    assert(channel.pop_or_wait(out, std::chrono::milliseconds(1)) ==
           SseServerChannel::PopResult::closed);

    SseServerChannel normalized(0, 0);
    assert(normalized.capacity() == 1);
    normalized.close();
    assert(normalized.push_framed("late") == SseServerChannel::PushResult::closed);
    return 0;
}
