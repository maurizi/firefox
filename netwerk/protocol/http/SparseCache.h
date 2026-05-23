/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_SparseCache_h_
#define mozilla_net_SparseCache_h_

// Coordinator classes and helpers for the sparse byte-range cache
// (bug 1615698). nsHttpChannel drives the overall request lifecycle; the
// helpers in this header handle the per-region async cache plumbing:
//
//   - SparseRegionReader: probes the regions of a multi-region or multi-range
//     request and, on full coverage, hands the windowed slice streams back
//     to the channel for serving from cache.
//   - SparseMetaResolver: resolves a suffix / open-ended sub-range request
//     against the per-URL :sparsemeta entry to learn the entity total.
//   - SparseMetaWriter: refreshes the per-URL :sparsemeta entry.
//   - SparseMultiRangePartWriter: writes each parsed multi-range part into
//     its region cache entry, serializing concurrent writers through the
//     SparseWriteQueue.
//   - SparseWriteQueue: main-thread-only serializer for per-region writes.
//   - SparseMultiRangeCaptureListener / SparseSingleRangeCaptureListener:
//     buffer the network response body so the channel can write it through
//     to region cache entries after OnStopRequest.
//
// The classes hold a back-reference to nsHttpChannel and call its public
// OnSparse* / MaybeWriteSparse* callbacks; the channel owns RefPtrs to the
// coordinators for the lifetime of each async operation.

#include <cstdint>
#include <functional>

#include "SparseMeta.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "nsCOMPtr.h"
#include "nsICacheEntryOpenCallback.h"
#include "nsIStreamListener.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsTHashMap.h"

class nsICacheEntry;
class nsICacheStorage;
class nsIURI;

namespace mozilla::net {

class nsHttpChannel;
class nsHttpResponseHead;

// Kind of single-spec byte-range request that needs the per-URL :sparsemeta
// entry to be resolved to absolute bounds before it can be served. Mapped
// from a Range header by ParseSingleSuffixOrOpenEndedByteRange.
enum class SparseMetaResolveKind : uint8_t { Suffix, OpenEnded };

// Probes the region entries of a multi-region sparse request. If every
// region's slice of the request is present, it multiplexes their windowed
// reads so the request can be served from cache (OnSparseSpanReady);
// otherwise it reports a miss (OnSparseSpanMiss) and the channel fetches
// the full range from network.
class SparseRegionReader final : public nsICacheEntryOpenCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICACHEENTRYOPENCALLBACK

  struct RegionSlice {
    int64_t mRegionId;
    int64_t mChildOffset;
    int64_t mSliceLen;
  };

  SparseRegionReader(nsHttpChannel* aChannel, nsICacheStorage* aStorage,
                     nsIURI* aURI, const nsACString& aIdExtensionBase,
                     nsTArray<RegionSlice>&& aRegions);

  nsresult Start();

 private:
  ~SparseRegionReader() = default;

  nsresult OpenRegion(uint32_t aIndex);
  void Fail();

  RefPtr<nsHttpChannel> mChannel;
  nsCOMPtr<nsICacheStorage> mStorage;
  nsCOMPtr<nsIURI> mURI;
  nsCString mIdExtensionBase;
  nsTArray<RegionSlice> mRegions;
  nsTArray<nsCOMPtr<nsIInputStream>> mStreams;
  nsCOMPtr<nsICacheEntry> mFirstEntry;
  // Strong validator (ETag, else Last-Modified) read off the first region's
  // stored head; subsequent regions must match or assembly is aborted to
  // avoid splicing bytes from different resource versions together.
  nsCString mFirstValidator;
  uint32_t mIndex = 0;
  bool mDone = false;
};

// Async resolver for suffix / open-ended sub-range requests. Opens the
// URL :sparsemeta entry read-only, reads the entity total + validator,
// and hands them back to the channel (which then dispatches into the
// normal region open path with bounded coordinates). On miss the channel
// falls through to its historical bypass + capture write-through.
class SparseMetaResolver final : public nsICacheEntryOpenCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICACHEENTRYOPENCALLBACK

  SparseMetaResolver(nsHttpChannel* aChannel, nsICacheStorage* aStorage,
                     nsIURI* aURI, const nsACString& aIdExtensionBase,
                     SparseMetaResolveKind aKind, int64_t aParam);

  nsresult Start();

 private:
  ~SparseMetaResolver() = default;

  RefPtr<nsHttpChannel> mChannel;
  nsCOMPtr<nsICacheStorage> mStorage;
  nsCOMPtr<nsIURI> mURI;
  nsCString mIdExtensionBase;
  SparseMetaResolveKind mKind;
  int64_t mParam;  // suffix length, or open-ended start
  bool mDone = false;
};

// Async writer for the per-URL ":sparsemeta" entry. Reads any prior meta on
// the entry, then writes a new record — bumping the diagnostic rev counter
// when the validator differs from what was stored. Idempotent across
// concurrent writers: the last writer's record wins on total/content-type
// (always identical for a given resource version) and rev moves
// monotonically.
class SparseMetaWriter final : public nsICacheEntryOpenCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICACHEENTRYOPENCALLBACK

  SparseMetaWriter(nsICacheStorage* aStorage, nsIURI* aURI,
                   const nsACString& aIdExtension,
                   SparseMeta::Record&& aRecord);

  nsresult Start();

 private:
  ~SparseMetaWriter() = default;

  nsCOMPtr<nsICacheStorage> mStorage;
  nsCOMPtr<nsIURI> mURI;
  nsCString mIdExtension;
  SparseMeta::Record mRecord;
};

// Per-region write coordinator for the sparse multi-range write-through
// path. cache2 only allows one open output stream per CacheFile at a time
// (CacheFile::OpenOutputStream's mOutput check), so two concurrent writers
// targeting the same URL :sparsechunk=N entry would collide and the loser
// silently drops its bytes. This main-thread-only queue serializes writes
// to the same key: the first TryAcquire(key) wins, subsequent ones queue
// and run sequentially as each predecessor calls Release(key). All callers
// are on the cache callback main thread (AsyncOpenURI's
// OnCacheEntryAvailable runs there).
class SparseWriteQueue {
 public:
  using ReadyCb = std::function<void()>;

  // Returns true if the caller acquired immediately; aCallback is unused
  // in that case. Returns false if queued — aCallback will be dispatched
  // on the current thread once the prior holder calls Release(aKey).
  // The caller MUST call Release(aKey) once its OpenOutputStream + write +
  // Close cycle completes (including every failure path).
  static bool TryAcquire(const nsACString& aKey, ReadyCb&& aCallback);
  // Acquire if free, otherwise return false WITHOUT queuing a callback.
  // For callers that can't tolerate async deferral (the normal cache-
  // listener path can't — it has to install the tee before bytes start
  // flowing) and prefer to take a different fallback when the slot is busy.
  static bool TryAcquireOrSkip(const nsACString& aKey);
  static void Release(const nsACString& aKey);

 private:
  struct State {
    bool mHeld = false;
    nsTArray<ReadyCb> mWaiters;
  };
  static nsTHashMap<nsCStringHashKey, State>& Map();
};

// Sequentially writes each parsed multi-range part to its region cache
// entry so a subsequent multi-range request can be served from cache.
class SparseMultiRangePartWriter final : public nsICacheEntryOpenCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICACHEENTRYOPENCALLBACK

  struct Part {
    int64_t mRegionId;
    int64_t mChildOffset;
    int64_t mFirst;
    int64_t mLast;
    int64_t mTotal;
    nsCString mBody;
  };

  SparseMultiRangePartWriter(nsICacheStorage* aStorage, nsIURI* aURI,
                             const nsACString& aIdExtensionBase,
                             nsHttpResponseHead* aResponseHead,
                             nsTArray<Part>&& aParts);

  nsresult Start();

 private:
  ~SparseMultiRangePartWriter() = default;

  nsresult OpenPart(uint32_t aIndex);
  void ReleaseAndAdvance();

  nsCOMPtr<nsICacheStorage> mStorage;
  nsCOMPtr<nsIURI> mURI;
  nsCString mIdExtensionBase;
  UniquePtr<nsHttpResponseHead> mResponseHead;
  nsTArray<Part> mParts;
  // Cache key of the region currently held in the SparseWriteQueue. Set
  // when OpenPart acquires; cleared when ReleaseAndAdvance fires.
  nsCString mCurrentKey;
  uint32_t mIndex = 0;
};

// Buffers the network multipart/byteranges body while forwarding it to the
// consumer, then hands the buffered body to the channel on OnStopRequest so
// each part can be written through into its region cache entry.
class SparseMultiRangeCaptureListener final : public nsIStreamListener {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSIREQUESTOBSERVER

  SparseMultiRangeCaptureListener(nsHttpChannel* aChannel,
                                  nsIStreamListener* aConsumer,
                                  const nsACString& aContentType);

 private:
  ~SparseMultiRangeCaptureListener() = default;

  RefPtr<nsHttpChannel> mChannel;
  nsCOMPtr<nsIStreamListener> mConsumer;
  nsCString mContentType;
  nsCString mBuffer;
  bool mCappedOut = false;
  // 16 MiB cap so a runaway multipart response can't grow the buffer
  // unboundedly; uncached on overflow (the consumer still gets the
  // response).
  static constexpr size_t kMaxBuffer = 16 * 1024 * 1024;
};

// Single-range counterpart of SparseMultiRangeCaptureListener: buffers a
// contiguous 206 response while forwarding it, and on stop hands the
// buffered body to the channel for write-through across the spanned region
// cache entries. Used for multi-region single-range misses, and for suffix
// / open-ended ranges where the entity total is only known from the
// response Content-Range.
class SparseSingleRangeCaptureListener final : public nsIStreamListener {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSIREQUESTOBSERVER

  SparseSingleRangeCaptureListener(nsHttpChannel* aChannel,
                                   nsIStreamListener* aConsumer);

 private:
  ~SparseSingleRangeCaptureListener() = default;

  RefPtr<nsHttpChannel> mChannel;
  nsCOMPtr<nsIStreamListener> mConsumer;
  nsCString mBuffer;
  bool mCappedOut = false;
  // Same 16 MiB cap as the multi-range capture (uncached on overflow).
  static constexpr size_t kMaxBuffer = 16 * 1024 * 1024;
};

// Free-function helpers ----------------------------------------------------

// Generates a multipart/byteranges boundary string. The boundary just needs
// to be unique within the response; a fixed prefix + a session-unique
// counter is sufficient (and stable across the test).
void GenerateBoundary(nsACString& aBoundary);

// Splits a contiguous body covering absolute bytes [aFirst..aLast] (with the
// resource total aTotal) into one Part per region it spans and appends them
// to aOut. Used both by the multipart parser (for parts that span regions)
// and by single-range write-through (multi-region miss; suffix/open-ended
// responses).
void PushRegionSlices(nsTArray<SparseMultiRangePartWriter::Part>& aOut,
                      const nsACString& aBody, int64_t aFirst, int64_t aLast,
                      int64_t aTotal, int64_t aChunkSize);

// Parses one part's headers from [aBody+aPos, ...). Advances *aPos past the
// CRLFCRLF header/body separator. Returns true and fills
// *aFirst/*aLast/*aTotal when a Content-Range header is found.
bool ParsePartHeaders(const nsCString& aBody, size_t* aPos, int64_t* aFirst,
                      int64_t* aLast, int64_t* aTotal);

// Parses aValue as a single suffix ("bytes=-N") or open-ended ("bytes=N-")
// byte range (case-insensitive; whitespace ignored). On a suffix returns
// {Suffix, N}; on open-ended returns {OpenEnded, N}. These can't be mapped
// to regions at request time without the entity total — meta resolution
// provides it on warm caches; otherwise the response's Content-Range
// supplies it and the bypass + capture path writes through.
bool ParseSingleSuffixOrOpenEndedByteRange(const nsACString& aValue,
                                           SparseMetaResolveKind* aOutKind,
                                           int64_t* aOutParam);

// True iff aValue is a single suffix or open-ended byte range.
bool IsSingleSuffixOrOpenEndedByteRange(const nsACString& aValue);

}  // namespace mozilla::net

#endif  // mozilla_net_SparseCache_h_
