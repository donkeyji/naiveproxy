// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef URL_URL_CANON_IP_H_
#define URL_URL_CANON_IP_H_

#include "base/component_export.h"
#include "base/strings/string_piece_forward.h"
#include "url/third_party/mozilla/url_parse.h"
#include "url/url_canon.h"

namespace url {

// Writes the given IPv4 address to |output|.
COMPONENT_EXPORT(URL)
void AppendIPv4Address(const unsigned char address[4], CanonOutput* output);

// Writes the given IPv6 address to |output|.
COMPONENT_EXPORT(URL)
void AppendIPv6Address(const unsigned char address[16], CanonOutput* output);

// Searches the host name for the portions of the IPv4 address. On success,
// each component will be placed into |components| and it will return true.
// It will return false if the host can not be separated as an IPv4 address
// or if there are any non-7-bit characters or other characters that can not
// be in an IP address. (This is important so we fail as early as possible for
// common non-IP hostnames.)
//
// Not all components may exist. If there are only 3 components, for example,
// the last one will have a length of -1 or 0 to indicate it does not exist.
//
// Note that many platforms' inet_addr will ignore everything after a space
// in certain circumstances if the stuff before the space looks like an IP
// address. IE6 is included in this. We do NOT handle this case. In many cases,
// the browser's canonicalization will get run before this which converts
// spaces to %20 (in the case of IE7) or rejects them (in the case of Mozilla),
// so this code path never gets hit. Our host canonicalization will notice
// these spaces and escape them, which will make IP address finding fail. This
// seems like better behavior than stripping after a space.
COMPONENT_EXPORT(URL)
bool FindIPv4Components(const char* spec,
                        const Component& host,
                        Component components[4]);
COMPONENT_EXPORT(URL)
bool FindIPv4Components(const char16_t* spec,
                        const Component& host,
                        Component components[4]);

// Converts an IPv4 address to a 32-bit number (network byte order).
//
// Possible return values:
//   IPV4    - IPv4 address was successfully parsed.
//   BROKEN  - Input was formatted like an IPv4 address, but overflow occurred
//             during parsing.
//   NEUTRAL - Input couldn't possibly be interpreted as an IPv4 address.
//             It might be an IPv6 address, or a hostname.
//
// On success, |num_ipv4_components| will be populated with the number of
// components in the IPv4 address.
COMPONENT_EXPORT(URL)
CanonHostInfo::Family IPv4AddressToNumber(const char* spec,
                                          const Component& host,
                                          unsigned char address[4],
                                          int* num_ipv4_components);
COMPONENT_EXPORT(URL)
CanonHostInfo::Family IPv4AddressToNumber(const char16_t* spec,
                                          const Component& host,
                                          unsigned char address[4],
                                          int* num_ipv4_components);

// Converts an IPv6 address to a 128-bit number (network byte order), returning
// true on success. False means that the input was not a valid IPv6 address.
//
// NOTE that |host| is expected to be surrounded by square brackets.
// i.e. "[::1]" rather than "::1".
COMPONENT_EXPORT(URL)
bool IPv6AddressToNumber(const char* spec,
                         const Component& host,
                         unsigned char address[16]);
COMPONENT_EXPORT(URL)
bool IPv6AddressToNumber(const char16_t* spec,
                         const Component& host,
                         unsigned char address[16]);

// Temporary enum for collecting histograms at the DNS and URL level about
// hostname validity, for potentially updating the URL spec.
//
// This is used in histograms, so old values should not be reused, and new
// values should be added at the bottom.
//
// TODO(https://crbug.com/1149194): Remove this once the bug is fixed.
enum class HostSafetyStatus {
  // Any canonical hostname that doesn't fit into any other class. IPv4
  // hostnames, hostnames that don't have numeric eTLDs, etc. Hostnames that are
  // broken are also considered OK.
  kOk = 0,

  // The top level domain looks numeric. This is basically means it either
  // parses as a number per the URL spec, or is entirely numeric ("09" doesn't
  // currently parse as a number, since the leading "0" indicates an octal
  // value).
  kTopLevelDomainIsNumeric = 1,

  // Both the top level domain and the next level domain look like a number,
  // using the above definition. This is the case that is actually concerning -
  // for these domains, the eTLD+1 is purely numeric, which means putting it as
  // the hostname of a URL will potentially result in an IPv4 hostname. This is
  // logically a subset of kTopLevelDomainIsNumeric, but when both apply, this
  // label will be returned instead.
  kTwoHighestLevelDomainsAreNumeric = 2,

  kMaxValue = kTwoHighestLevelDomainsAreNumeric,
};

// Calculates the HostSafetyStatus of a hostname. Hostname should have been
// canonicalized. This function is only intended to be temporary, to inform
// decisions around tightening up what the URL parser considers valid hostnames.
//
// TODO(https://crbug.com/1149194): Remove this once the bug is fixed.
COMPONENT_EXPORT(URL)
HostSafetyStatus CheckHostnameSafety(const char* hostname,
                                     const Component& host);
COMPONENT_EXPORT(URL)
HostSafetyStatus CheckHostnameSafety(const char16_t* hostname,
                                     const Component& host);

}  // namespace url

#endif  // URL_URL_CANON_IP_H_
