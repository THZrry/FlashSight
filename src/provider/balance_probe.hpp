// balance_probe.hpp - "how much credit is left" per provider
//
// There is no standard: every vendor that offers a balance API uses its own
// private endpoint with its own JSON shape. The vendor is therefore identified by
// the ENDPOINT the user configured (a provider renamed "My API" still points at
// api.deepseek.com), and vendors with no public API are reported as such instead
// of being guessed at.
//
// Kept out of FsApi so it can be unit-tested against a stub NetClient: the parse
// of each vendor's reply is the part that must not be wrong, and a wrong balance
// is worse than an honest failure.
#pragma once

#include <string>

#include "common/fs_net.hpp"
#include "provider/provider_types.hpp"

namespace fs {

struct BalanceProbe {
  // "ok"          - balance/used/limit hold real numbers
  // "unsupported" - this vendor has no public balance endpoint
  // "failed"      - it has one but the query failed; `note` says why
  std::string status = "failed";
  std::string currency;
  std::string balance;
  std::string used;
  std::string limit;
  std::string note;
};

// Query one provider. Never throws; a network/parse/HTTP error becomes
// status="failed" with the reason in `note`.
BalanceProbe probe_balance(const ProviderConn& conn, const NetClient* net);

}  // namespace fs
