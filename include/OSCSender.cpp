#include "plugin.hpp"
#include <atomic>
#include <iterator>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/lockfree/queue.hpp>
#include <exception>
#include <iostream>
#include <array>
#include <stdexcept>
#include <stdio.h>
#include <thread>
#include <memory>
#include <unistd.h>
#include <sys/time.h>
#include <cstddef>
#include <oscpp/client.hpp>

#include <nonstd/optional.hpp>

#define MICROS_PER_SEC         1000000
#define us2s(x) (((double)x)/(double)MICROS_PER_SEC)

// struct timeval tv;
// gettimeofday(&tv, NULL);

uint64_t formatTime(timeval tv) {
  return (
    ((uint64_t) tv.tv_sec + 2208988800L) << 32 |
    ((uint32_t) (us2s(tv.tv_usec) * (double)4294967296L))
  );
}

struct OSCMessageValue {
  enum {FLOAT, INT, STRING} type;
  union {
    float f;
    int i;
    char* s;
  };
};

struct OSCMessage {
  std::string address;
  size_t valuesSize;
  OSCMessageValue* values;
};

struct OSCBundle {
  timeval time;
  size_t messagesSize;
  OSCMessage* messages;
};

size_t makePacket(void* buffer, size_t size, const OSCBundle& bundle) {
  OSCPP::Client::Packet packet(buffer, size);
  packet = packet.openBundle(formatTime(bundle.time));
  for (size_t i = 0; i < bundle.messagesSize; i++) {
    auto msg = bundle.messages[i];
    packet = packet.openMessage(msg.address.c_str(), msg.valuesSize);

    for (size_t j = 0; j < msg.valuesSize; j++) {
      auto val = msg.values[j];
      switch (val.type) {
        case OSCMessageValue::FLOAT:
          packet = packet.float32(val.f);
          break;
        case OSCMessageValue::INT:
          packet = packet.int32(val.i);
          break;
        case OSCMessageValue::STRING:
          packet = packet.string(val.s);
          break;
        default:
          throw std::invalid_argument("Message type not supported");
      }
    }
    packet.closeMessage();
  }
  packet.closeBundle();
  return packet.size();
}

using boost::asio::ip::udp;

const size_t kMaxPacketSize = 8192;

class OSCSender final {
  public:
  OSCSender():
    _io_service(),
    _is_running(false),
    _socket(_io_service),
    _endpoint(nonstd::nullopt) {
  }

  OSCSender(
    udp::endpoint endpoint
  ):
    _io_service(),
    _is_running(false),
    _socket(_io_service),
    _endpoint(endpoint) {
  }

  OSCSender(
    const OSCSender& pOther
  ):
    _io_service(),
    _is_running(false),
    _socket(_io_service),
    _endpoint(pOther._endpoint) {
  }

  ~OSCSender() {
    if (_is_running.load(std::memory_order_relaxed)) stop();
  }

  void setEndpoint(udp::endpoint endpoint) {
    _endpoint = endpoint;
  }

  void start() {
    DEBUG("starting...");
    assert(!_is_running.exchange(true, std::memory_order_relaxed));
    DEBUG("started");
    _socket.open(udp::v4());

    _io_thread = new std::thread([&] () {
      using work_guard_t = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type
      >;
      work_guard_t work_guard(_io_service.get_executor());
      _io_service.run();
    });

    _watchdog_thread = new std::thread([&] {
      while (_is_running.load(std::memory_order_relaxed)) {
      }
      _io_service.stop();
    });
  }

  void send(OSCBundle data) {
    if (!_is_running.load(std::memory_order_relaxed)) return;

    size_t size = makePacket(&_buffer, kMaxPacketSize, data);
    // delete[] data.messages;

    if (!_endpoint.has_value()) {
      return;
    }

    _socket.async_send_to(
      boost::asio::buffer(_buffer, size),
      _endpoint.value(),
      0,
      [=] (
        boost::system::error_code error,
        std::size_t bytesTransferred
      ) {
        if (!!error.value()) {
          DEBUG("error sending message %s", error.message().c_str());
        }
      }
    );
  }

  void stop() {
    if (_is_running.exchange(false, std::memory_order_relaxed)) return;

    if (_io_thread != nullptr && _io_thread->joinable()) {
      _io_thread->join();
      delete _io_thread;
      _io_thread = nullptr;
    }

    if (_watchdog_thread != nullptr && _watchdog_thread->joinable()) {
      _watchdog_thread->join();
      delete _watchdog_thread;
      _watchdog_thread = nullptr;
    }

    if (_socket.is_open()) {
      _socket.close();
    }
  }

  private:
  boost::asio::io_service _io_service;
  std::atomic<bool> _is_running;
  std::thread* _io_thread = nullptr;
  std::thread* _watchdog_thread = nullptr;
  udp::socket _socket;
  nonstd::optional<udp::endpoint> _endpoint;
  std::array<char, kMaxPacketSize> _buffer;
};
