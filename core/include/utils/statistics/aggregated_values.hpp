#pragma once

#include <array>
#include <atomic>

#include <fmt/format.h>

#include <formats/json/value_builder.hpp>
#include <utils/assert.hpp>
#include <utils/datetime.hpp>

namespace utils::statistics {

/* Timing values aggregated into buckets, each bucket is from 2**k to 2**(k+1)
 * Values out of range are put into last bucket
 */
template <size_t Length>
struct AggregatedValues {
  std::array<std::atomic_llong, Length> value{{}};

  AggregatedValues& operator=(const AggregatedValues& other) {
    for (size_t i = 0; i < value.size(); ++i) value[i] = other.value[i].load();
    return *this;
  }

  AggregatedValues& operator+=(const AggregatedValues& other) {
    for (size_t i = 0; i < value.size(); ++i) value[i] += other.value[i].load();
    return *this;
  }

  void Add(size_t key, size_t value);

  long long Get(size_t bucket) const;
};

template <size_t Length>
void AggregatedValues<Length>::Add(size_t key, size_t delta) {
  size_t bucket = 0;
  while (key > 1) {
    bucket++;
    key >>= 1;
  }
  if (bucket >= Length) bucket = Length - 1;

  value[bucket] += delta;
}

template <size_t Length>
long long AggregatedValues<Length>::Get(size_t bucket) const {
  UASSERT(bucket < value.size());
  return value[bucket].load();
}

/// TODO: convert to histogram for Solomon
template <size_t length>
formats::json::Value AggregatedValuesToJson(
    const AggregatedValues<length>& stats, const std::string& suffix = {}) {
  formats::json::ValueBuilder result(formats::json::Type::kObject);
  for (size_t i = 0; i < length; ++i) {
    auto l = i ? (1 << i) : 0;
    auto r = (1 << (i + 1)) - 1;

    std::string key;
    if (i != length - 1) {
      key = fmt::format("{}-{}{}", l, r, suffix);
    } else {
      key = fmt::format("{}-x{}", l, suffix);
    }
    result[key] = stats.value[i].load();
  }
  return result.ExtractValue();
}

template <size_t Length>
formats::json::Value AggregatedValuesToJson(
    const AggregatedValues<Length>& stats) {
  formats::json::ValueBuilder result(formats::json::Type::kObject);
  for (size_t i = 0; i < Length; ++i) {
    auto l = i ? (1 << i) : 0;
    auto r = (1 << (i + 1)) - 1;
    std::string key;
    if (i < Length - 1) {
      key = fmt::format("{}-{}", l, r);
    } else {
      key = fmt::format("{}-x", l);
    }
    result[key] = stats.Get(i);
  }
  return result.ExtractValue();
}

}  // namespace utils::statistics