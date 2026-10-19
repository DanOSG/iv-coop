#pragma once
// Minimal little-endian byte (de)serialisation. Quantisation/bit packing comes later; for
// now readability beats size.

#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

namespace ivc::net {

class Writer {
 public:
  template <class T>
    requires std::is_trivially_copyable_v<T>
  Writer& put(const T& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    buf_.insert(buf_.end(), p, p + sizeof(T));
    return *this;
  }
  std::vector<std::uint8_t>& data() { return buf_; }

 private:
  std::vector<std::uint8_t> buf_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> d) : d_(d) {}

  template <class T>
    requires std::is_trivially_copyable_v<T>
  bool get(T& out) {
    if (pos_ + sizeof(T) > d_.size()) return ok_ = false;
    std::memcpy(&out, d_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }
  bool ok() const { return ok_; }

 private:
  std::span<const std::uint8_t> d_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

} // namespace ivc::net
