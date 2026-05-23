/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CacheFileUtils_h_
#define CacheFileUtils_h_

#include "nsError.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsTArray.h"
#include "mozilla/Mutex.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/TimeStamp.h"

class nsILoadContextInfo;

namespace mozilla {
namespace net {
namespace CacheFileUtils {

extern const char* kAltDataKey;

// Metadata key under which a sparse (partially-filled) entry's validity ranges
// are persisted. Only present when the entry has holes.
extern const char* kSparseRangesKey;

already_AddRefed<nsILoadContextInfo> ParseKey(const nsACString& aKey,
                                              nsACString* aIdEnhance = nullptr,
                                              nsACString* aURISpec = nullptr);

void AppendKeyPrefix(nsILoadContextInfo* aInfo, nsACString& _retval);

void AppendTagWithValue(nsACString& aTarget, char const aTag,
                        const nsACString& aValue);

nsresult KeyMatchesLoadContextInfo(const nsACString& aKey,
                                   nsILoadContextInfo* aInfo, bool* _retval);

class ValidityPair {
 public:
  ValidityPair(uint32_t aOffset, uint32_t aLen);

  ValidityPair& operator=(const ValidityPair& aOther) = default;

  // Returns true when two pairs can be merged, i.e. they do overlap or the one
  // ends exactly where the other begins.
  bool CanBeMerged(const ValidityPair& aOther) const;

  // Returns true when aOffset is placed anywhere in the validity interval or
  // exactly after its end.
  bool IsInOrFollows(uint32_t aOffset) const;

  // Returns true when this pair has lower offset than the other pair. In case
  // both pairs have the same offset it returns true when this pair has a
  // shorter length.
  bool LessThan(const ValidityPair& aOther) const;

  // Merges two pair into one.
  void Merge(const ValidityPair& aOther);

  uint32_t Offset() const { return mOffset; }
  uint32_t Len() const { return mLen; }

 private:
  uint32_t mOffset;
  uint32_t mLen;
};

class ValidityMap {
 public:
  // Prints pairs in the map into log.
  void Log() const;

  // Returns number of pairs in the map.
  uint32_t Length() const;

  // Adds a new pair to the map. It keeps the pairs ordered and merges pairs
  // when possible.
  void AddPair(uint32_t aOffset, uint32_t aLen);

  // Removes all pairs from the map.
  void Clear();

  size_t SizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  ValidityPair& operator[](uint32_t aIdx);

 private:
  nsTArray<ValidityPair> mMap;
};

// Tracks which byte ranges of a (possibly sparse) cache entry actually contain
// data. A normal entry is contiguous [0, dataSize); a sparse entry — used for
// the child entries of HTTP byte-range caching — may have holes. Offsets are
// entry-relative (within this one entry's data), 64-bit, kept sorted, disjoint
// and non-adjacent (touching ranges merge). Persisted in entry metadata so
// holes survive across sessions. Distinct from the transient per-chunk
// ValidityMap (32-bit, intra-chunk read/write merge): this is whole-entry and
// persistent, and byte-exact (validity is tracked as ranges, not blocks).
class SparseRangeMap {
 public:
  // Adds [aOffset, aOffset + aLen) to the map, merging overlapping/touching
  // ranges. A non-positive length is ignored.
  void AddRange(int64_t aOffset, int64_t aLen);

  // True when [aOffset, aOffset + aLen) is entirely within a single stored
  // range. A non-positive length is trivially covered.
  bool Covers(int64_t aOffset, int64_t aLen) const;

  // The first contiguous available run at or after aOffset. Sets *aStart to
  // where the run begins (== aOffset if data is present there, otherwise the
  // start of the next run), *aLength to its length, and returns true; returns
  // false when there is no data at or after aOffset.
  bool FirstAvailableRange(int64_t aOffset, int64_t* aStart,
                           int64_t* aLength) const;

  // The first byte at or after aOffset that is NOT present. Equals aOffset when
  // aOffset itself is in a hole (or beyond all ranges).
  int64_t FirstHoleAfter(int64_t aOffset) const;

  // Sum of all range lengths (bytes actually present, excluding holes).
  int64_t ValidBytes() const;

  uint32_t Length() const { return mRanges.Length(); }

  // True when the map is a single contiguous range [0, aDataSize) (or empty
  // with aDataSize == 0): an ordinary, non-sparse entry needing no metadata.
  bool IsContiguousFromZero(int64_t aDataSize) const;

  // Drops everything at or after aOffset, clipping a range that straddles it.
  void Truncate(int64_t aOffset);

  void Clear() { mRanges.Clear(); }

  // Canonical ASCII "offset,length;" pairs.
  void Serialize(nsACString& aOutput) const;
  // Tolerant of malformed input: stops at the first bad token. Clears first.
  void Parse(const nsACString& aInput);

  void Log() const;
  size_t SizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  struct Range {
    int64_t mOffset;
    int64_t mLen;
    int64_t End() const { return mOffset + mLen; }
  };
  nsTArray<Range> mRanges;
};

class DetailedCacheHitTelemetry {
 public:
  enum ERecType { HIT = 0, MISS = 1 };

  static void AddRecord(ERecType aType, TimeStamp aLoadStart);

 private:
  class HitRate {
   public:
    HitRate() = default;

    void AddRecord(ERecType aType);
    uint32_t GetHitRateBucket() const;
    uint32_t Count();
    void Reset();

   private:
    uint32_t mHitCnt = 0;
    uint32_t mMissCnt = 0;
  };

  // Group the hits and misses statistics by cache files count ranges (0-5000,
  // 5001-10000, ... , 95001- )
  static const uint32_t kRangeSize = 5000;
  static const uint32_t kNumOfRanges = 20;
  static const uint32_t kPercentageRange = 5;
  static const uint32_t kMaxPercentage = 100;

  // Use the same ranges to report an average hit rate. Report the hit rates
  // (and reset the counters) every kTotalSamplesReportLimit samples.
  static const uint32_t kTotalSamplesReportLimit = 1000;

  // Report hit rate for a given cache size range only if it contains
  // kHitRateSamplesReportLimit or more samples. This limit should avoid
  // reporting a biased statistics.
  static const uint32_t kHitRateSamplesReportLimit = 500;

  // All hit rates are accumulated in a single telemetry probe, so to use
  // a sane number of enumerated values the hit rate is divided into buckets
  // instead of using a percent value. This constant defines number of buckets
  // that we divide the hit rates into. I.e. we'll report ranges 0%-5%, 5%-10%,
  // 10-%15%, ...
  static const uint32_t kHitRateBuckets = 20;

  // Protects sRecordCnt, sHRStats and Telemetry::Accumulated() calls.
  static StaticMutex sLock;

  // Counter of samples that is compared against kTotalSamplesReportLimit.
  static uint32_t sRecordCnt MOZ_GUARDED_BY(sLock);

  // Hit rate statistics for every cache size range.
  static HitRate sHRStats[kNumOfRanges] MOZ_GUARDED_BY(sLock);
};

void FreeBuffer(void* aBuf);

nsresult ParseAlternativeDataInfo(const char* aInfo, int64_t* _offset,
                                  nsACString* _type);

void BuildAlternativeDataInfo(const char* aInfo, int64_t aOffset,
                              nsACString& _retval);

class CacheFileLock final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CacheFileLock)
  CacheFileLock() = default;

  mozilla::Mutex& Lock() MOZ_RETURN_CAPABILITY(mLock) { return mLock; }

 private:
  ~CacheFileLock() = default;

  mozilla::Mutex mLock{"CacheFile.mLock"};
};

}  // namespace CacheFileUtils
}  // namespace net
}  // namespace mozilla

#endif
