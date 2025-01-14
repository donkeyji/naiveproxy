// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_CERT_CERT_VERIFY_PROC_BUILTIN_H_
#define NET_CERT_CERT_VERIFY_PROC_BUILTIN_H_

#include <memory>

#include "base/memory/ref_counted.h"
#include "base/supports_user_data.h"
#include "net/base/net_export.h"
#include "net/der/parse_values.h"

namespace net {

class CertNetFetcher;
class CertVerifyProc;
class SystemTrustStore;

class NET_EXPORT CertVerifyProcBuiltinResultDebugData
    : public base::SupportsUserData::Data {
 public:
  CertVerifyProcBuiltinResultDebugData(
      base::Time verification_time,
      const der::GeneralizedTime& der_verification_time);

  static const CertVerifyProcBuiltinResultDebugData* Get(
      const base::SupportsUserData* debug_data);
  static void Create(base::SupportsUserData* debug_data,
                     base::Time verification_time,
                     const der::GeneralizedTime& der_verification_time);

  // base::SupportsUserData::Data implementation:
  std::unique_ptr<Data> Clone() override;

  base::Time verification_time() const { return verification_time_; }
  const der::GeneralizedTime& der_verification_time() const {
    return der_verification_time_;
  }

 private:
  base::Time verification_time_;
  der::GeneralizedTime der_verification_time_;
};

// TODO(crbug.com/649017): This is not how other cert_verify_proc_*.h are
// implemented -- they expose the type in the header. Use a consistent style
// here too.
NET_EXPORT scoped_refptr<CertVerifyProc> CreateCertVerifyProcBuiltin(
    scoped_refptr<CertNetFetcher> net_fetcher,
    std::unique_ptr<SystemTrustStore> system_trust_store);

// Returns the time limit used by CertVerifyProcBuiltin. Intended for test use.
NET_EXPORT_PRIVATE base::TimeDelta
GetCertVerifyProcBuiltinTimeLimitForTesting();

}  // namespace net

#endif  // NET_CERT_CERT_VERIFY_PROC_BUILTIN_H_
