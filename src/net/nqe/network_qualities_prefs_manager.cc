// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/nqe/network_qualities_prefs_manager.h"

#include <string>
#include <utility>

#include "base/bind.h"
#include "base/metrics/histogram_macros_local.h"
#include "base/rand_util.h"
#include "base/sequenced_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/nqe/network_quality_estimator.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace net {

namespace {

// Maximum size of the prefs that hold the qualities of different networks.
// A single entry in the cache consists of three tuples:
// (i)   SSID or MCCMNC of the network. SSID is at most 32 characters in length
//       (but is typically shorter than that). MCCMNC is at most 6 characters
//       long.
// (ii)  Connection type of the network as reported by network
//       change notifier (an enum).
// (iii) Effective connection type of the network (an enum).
constexpr size_t kMaxCacheSize = 20u;

// Parses |value| into a map of NetworkIDs and CachedNetworkQualities,
// and returns the map.
ParsedPrefs ConvertDictionaryValueToMap(const base::DictionaryValue* value) {
  DCHECK_GE(kMaxCacheSize, value->DictSize());

  ParsedPrefs read_prefs;
  for (const auto& it : value->DictItems()) {
    nqe::internal::NetworkID network_id =
        nqe::internal::NetworkID::FromString(it.first);

    std::string effective_connection_type_string;
    const bool effective_connection_type_available =
        it.second.GetAsString(&effective_connection_type_string);
    DCHECK(effective_connection_type_available);

    absl::optional<EffectiveConnectionType> effective_connection_type =
        GetEffectiveConnectionTypeForName(effective_connection_type_string);
    DCHECK(effective_connection_type.has_value());

    nqe::internal::CachedNetworkQuality cached_network_quality(
        effective_connection_type.value_or(EFFECTIVE_CONNECTION_TYPE_UNKNOWN));
    read_prefs[network_id] = cached_network_quality;
  }
  return read_prefs;
}

}  // namespace

NetworkQualitiesPrefsManager::NetworkQualitiesPrefsManager(
    std::unique_ptr<PrefDelegate> pref_delegate)
    : pref_delegate_(std::move(pref_delegate)),
      prefs_(pref_delegate_->GetDictionaryValue()),
      network_quality_estimator_(nullptr) {
  DCHECK(pref_delegate_);
  DCHECK_GE(kMaxCacheSize, prefs_->DictSize());
}

NetworkQualitiesPrefsManager::~NetworkQualitiesPrefsManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ShutdownOnPrefSequence();

  if (network_quality_estimator_)
    network_quality_estimator_->RemoveNetworkQualitiesCacheObserver(this);
}

void NetworkQualitiesPrefsManager::InitializeOnNetworkThread(
    NetworkQualityEstimator* network_quality_estimator) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(network_quality_estimator);

  // Read |prefs_| again since they have now been fully initialized. This
  // overwrites any values that may have been added to |prefs_| since
  // construction of |this| via OnChangeInCachedNetworkQuality(). However, it's
  // expected that InitializeOnNetworkThread will be called soon after
  // construction of |this|. So, any loss of values would be minimal.
  prefs_ = pref_delegate_->GetDictionaryValue();
  read_prefs_startup_ = ConvertDictionaryValueToMap(prefs_.get());

  network_quality_estimator_ = network_quality_estimator;
  network_quality_estimator_->AddNetworkQualitiesCacheObserver(this);

  // Notify network quality estimator of the read prefs.
  network_quality_estimator_->OnPrefsRead(read_prefs_startup_);
}

void NetworkQualitiesPrefsManager::ShutdownOnPrefSequence() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pref_delegate_.reset();
}

void NetworkQualitiesPrefsManager::ClearPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOCAL_HISTOGRAM_COUNTS_100("NQE.PrefsSizeOnClearing", prefs_->DictSize());
  prefs_->Clear();
  DCHECK_EQ(0u, prefs_->DictSize());
  pref_delegate_->SetDictionaryValue(*prefs_);
}

void NetworkQualitiesPrefsManager::OnChangeInCachedNetworkQuality(
    const nqe::internal::NetworkID& network_id,
    const nqe::internal::CachedNetworkQuality& cached_network_quality) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_GE(kMaxCacheSize, prefs_->DictSize());

  std::string network_id_string = network_id.ToString();

  // If the network ID contains a period, then return early since the dictionary
  // prefs cannot contain period in the path.
  if (network_id_string.find('.') != std::string::npos)
    return;

  prefs_->SetString(network_id_string,
                    GetNameForEffectiveConnectionType(
                        cached_network_quality.effective_connection_type()));

  if (prefs_->DictSize() > kMaxCacheSize) {
    // Delete one randomly selected value that has a key that is different from
    // |network_id|.
    DCHECK_EQ(kMaxCacheSize + 1, prefs_->DictSize());
    // Generate a random number in the range [0, |kMaxCacheSize| - 1] since the
    // number of network IDs in |prefs_| other than |network_id| is
    // |kMaxCacheSize|.
    int index_to_delete = base::RandInt(0, kMaxCacheSize - 1);

    for (const auto& it : prefs_->DictItems()) {
      // Delete the kth element in the dictionary, not including the element
      // that represents the current network. k == |index_to_delete|.
      if (nqe::internal::NetworkID::FromString(it.first) == network_id)
        continue;

      if (index_to_delete == 0) {
        prefs_->RemoveKey(it.first);
        break;
      }
      index_to_delete--;
    }
  }
  DCHECK_GE(kMaxCacheSize, prefs_->DictSize());

  // Notify the pref delegate so that it updates the prefs on the disk.
  pref_delegate_->SetDictionaryValue(*prefs_);
}

ParsedPrefs NetworkQualitiesPrefsManager::ForceReadPrefsForTesting() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::unique_ptr<base::DictionaryValue> value(
      pref_delegate_->GetDictionaryValue());
  return ConvertDictionaryValueToMap(value.get());
}

}  // namespace net
