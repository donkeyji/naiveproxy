// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DNS_DNS_TRANSACTION_H_
#define NET_DNS_DNS_TRANSACTION_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "net/base/request_priority.h"
#include "net/dns/public/secure_dns_mode.h"
#include "net/dns/record_rdata.h"
#include "url/gurl.h"

namespace net {

class DnsResponse;
class DnsSession;
class NetLogWithSource;
class ResolveContext;

// DnsTransaction implements a stub DNS resolver as defined in RFC 1034.
// The DnsTransaction takes care of retransmissions, name server fallback (or
// round-robin), suffix search, and simple response validation ("does it match
// the query") to fight poisoning.
//
// Destroying DnsTransaction cancels the underlying network effort.
class NET_EXPORT_PRIVATE DnsTransaction {
 public:
  virtual ~DnsTransaction() {}

  // Returns the original |hostname|.
  virtual const std::string& GetHostname() const = 0;

  // Returns the |qtype|.
  virtual uint16_t GetType() const = 0;

  // Starts the transaction.  Always completes asynchronously.
  virtual void Start() = 0;

  virtual void SetRequestPriority(RequestPriority priority) = 0;
};

// Startable/Cancellable object to represent a DNS probe sequence.
class DnsProbeRunner {
 public:
  // Destruction cancels the probes.
  virtual ~DnsProbeRunner() {}

  // Starts all applicable probes that are not already running. May be called
  // multiple times, but should not be called after destruction of the
  // DnsTransactionFactory.
  //
  // Set |network_change| to indicate if this start or restart was triggered by
  // a network connection change. Only used for logging and metrics.
  virtual void Start(bool network_change) = 0;

  // Gets the delay until the next scheduled probe to the specified DoH server.
  // Returns base::TimeDelta() if no probe scheduled.
  virtual base::TimeDelta GetDelayUntilNextProbeForTest(
      size_t doh_server_index) const = 0;
};

// Creates DnsTransaction which performs asynchronous DNS search.
// It does NOT perform caching, aggregation or prioritization of transactions.
//
// Destroying the factory does NOT affect any already created DnsTransactions.
//
// DnsProbeRunners, however, will safely abort themselves on destruction of
// their creating factory, and they should only be started or restarted while
// the factory is still alive.
class NET_EXPORT_PRIVATE DnsTransactionFactory {
 public:
  // Called with the response or NULL if no matching response was received.
  // Note that the |GetDottedName()| of the response may be different than the
  // original |hostname| as a result of suffix search.
  //
  // The |doh_provider_id| contains the provider ID for histograms of the last
  // DoH server attempted. If the name is unavailable, or this is not a DoH
  // transaction, |doh_provider_id| is nullopt.
  typedef base::OnceCallback<void(DnsTransaction* transaction,
                                  int neterror,
                                  const DnsResponse* response,
                                  absl::optional<std::string> doh_provider_id)>
      CallbackType;

  DnsTransactionFactory();
  virtual ~DnsTransactionFactory();

  // Creates DnsTransaction for the given |hostname| and |qtype| (assuming
  // QCLASS is IN). |hostname| should be in the dotted form. A dot at the end
  // implies the domain name is fully-qualified and will be exempt from suffix
  // search. |hostname| should not be an IP literal.
  //
  // The transaction will run |callback| upon asynchronous completion.
  // The |net_log| is used as the parent log.
  //
  // |secure| specifies whether DNS lookups should be performed using DNS-over-
  // HTTPS (DoH) or using plaintext DNS.
  //
  // When |fast_timeout| is true, the transaction will timeout quickly after
  // making its DNS attempts, without necessarily waiting long enough to allow
  // slower-than-average requests to complete. Intended as an optimization for
  // cases where the caller has reasonable fallback options to the transaction
  // and it would be beneficial to move on to those options sooner on signals
  // that the transaction is potentially slow or problematic.
  virtual std::unique_ptr<DnsTransaction> CreateTransaction(
      const std::string& hostname,
      uint16_t qtype,
      CallbackType callback,
      const NetLogWithSource& net_log,
      bool secure,
      SecureDnsMode secure_dns_mode,
      ResolveContext* resolve_context,
      bool fast_timeout) WARN_UNUSED_RESULT = 0;

  // Creates a runner to run the DoH probe sequence for all configured DoH
  // resolvers.
  virtual std::unique_ptr<DnsProbeRunner> CreateDohProbeRunner(
      ResolveContext* resolve_context) WARN_UNUSED_RESULT = 0;

  // The given EDNS0 option will be included in all DNS queries performed by
  // transactions from this factory.
  virtual void AddEDNSOption(const OptRecordRdata::Opt& opt) = 0;

  // Returns the default SecureDnsMode in the config.
  virtual SecureDnsMode GetSecureDnsModeForTest() = 0;

  // Creates a DnsTransactionFactory which creates DnsTransactionImpl using the
  // |session|.
  static std::unique_ptr<DnsTransactionFactory> CreateFactory(
      DnsSession* session) WARN_UNUSED_RESULT;

  base::WeakPtrFactory<DnsTransactionFactory> weak_factory_{this};
};

}  // namespace net

#endif  // NET_DNS_DNS_TRANSACTION_H_
