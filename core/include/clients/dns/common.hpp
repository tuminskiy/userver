#pragma once

/// @file clients/dns/common.hpp
/// @brief Common DNS client declarations

#include <boost/container/small_vector.hpp>

#include <engine/io/addr.hpp>

/// DNS client
namespace clients::dns {

using AddrVector = boost::container::small_vector<engine::io::Addr, 4>;

/// Checks whether the domain can be resolved (one of INET domains or
/// an unspecified one).
void CheckSupportedDomain(engine::io::AddrDomain);

}  // namespace clients::dns