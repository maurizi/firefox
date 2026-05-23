/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "HttpLog.h"

#include "SparseCache.h"

#include <algorithm>

#include "mozilla/Atomics.h"
#include "mozilla/SlicedInputStream.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/UniquePtr.h"
#ifndef ANDROID
#  include "mozilla/glean/NetwerkProtocolHttpMetrics.h"
#endif
#include "nsHttp.h"
#include "nsHttpChannel.h"
#include "nsHttpResponseHead.h"
#include "nsICacheEntry.h"
#include "nsICacheStorage.h"
#include "nsString.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"
#include "nsThreadUtils.h"
#include "prtime.h"

namespace mozilla::net {

// --- SparseRegionReader ---------------------------------------------------

NS_IMPL_ISUPPORTS(SparseRegionReader, nsICacheEntryOpenCallback)

SparseRegionReader::SparseRegionReader(nsHttpChannel* aChannel,
                                       nsICacheStorage* aStorage, nsIURI* aURI,
                                       const nsACString& aIdExtensionBase,
                                       nsTArray<RegionSlice>&& aRegions)
    : mChannel(aChannel),
      mStorage(aStorage),
      mURI(aURI),
      mIdExtensionBase(aIdExtensionBase),
      mRegions(std::move(aRegions)) {}

nsresult SparseRegionReader::Start() { return OpenRegion(0); }

nsresult SparseRegionReader::OpenRegion(uint32_t aIndex) {
  mIndex = aIndex;
  nsAutoCString ext(mIdExtensionBase);
  ext.AppendLiteral(":sparsechunk=");
  ext.AppendInt(mRegions[aIndex].mRegionId);
  return mStorage->AsyncOpenURI(mURI, ext, nsICacheStorage::OPEN_READONLY,
                                this);
}

void SparseRegionReader::Fail() {
  if (mDone) {
    return;
  }
  mDone = true;
  RefPtr<nsHttpChannel> chan = mChannel;
  chan->OnSparseSpanMiss();
}

NS_IMETHODIMP SparseRegionReader::OnCacheEntryCheck(nsICacheEntry* aEntry,
                                                    uint32_t* aResult) {
  const RegionSlice& r = mRegions[mIndex];
  bool cached = false;
  nsresult rv = aEntry->IsRangeCached(r.mChildOffset, r.mSliceLen, &cached);
  *aResult = (NS_SUCCEEDED(rv) && cached)
                 ? nsICacheEntryOpenCallback::ENTRY_WANTED
                 : nsICacheEntryOpenCallback::ENTRY_NOT_WANTED;
  return NS_OK;
}

NS_IMETHODIMP SparseRegionReader::OnCacheEntryAvailable(nsICacheEntry* aEntry,
                                                        bool aNew,
                                                        nsresult aStatus) {
  if (mDone) {
    return NS_OK;
  }

  const RegionSlice& r = mRegions[mIndex];
  bool covered = false;
  if (NS_SUCCEEDED(aStatus) && aEntry && !aNew) {
    aEntry->IsRangeCached(r.mChildOffset, r.mSliceLen, &covered);
  }
  if (!covered) {
    Fail();
    return NS_OK;
  }

  // Freshness: reject expired entries so multi-region serves don't silently
  // return stale content. Mirrors the check in OnCacheEntryCheckSparse for
  // the single-region path.
  uint32_t expirationTime = nsICacheEntry::NO_EXPIRATION_TIME;
  (void)aEntry->GetExpirationTime(&expirationTime);
  if (expirationTime != nsICacheEntry::NO_EXPIRATION_TIME) {
    uint32_t now = uint32_t(PR_Now() / PR_USEC_PER_SEC);
    if (expirationTime <= now) {
      Fail();
      return NS_OK;
    }
  }

  // Prefer the cheap stamped validator (set by the write-through paths);
  // fall back to parsing the stored head. Cache-Control: no-cache /
  // no-store / must-revalidate aren't checked here -- the URL channel
  // enforces those upstream before we reach the sparse path.
  nsAutoCString validator;
  nsCString stamped;
  nsresult srv = aEntry->GetMetaDataElement(SparseMeta::kRegionValidator,
                                            getter_Copies(stamped));
  bool haveStamp = NS_SUCCEEDED(srv) && !stamped.IsEmpty();

  nsHttpResponseHead head;
  bool headParsed =
      NS_SUCCEEDED(nsHttp::GetHttpResponseHeadFromCacheEntry(aEntry, &head));
  if (haveStamp) {
    validator = stamped;
  } else if (headParsed) {
    SparseMeta::ValidatorKind kind = SparseMeta::ValidatorKind::None;
    (void)SparseMeta::ExtractValidator(&head, validator, kind);
  } else {
    Fail();
    return NS_OK;
  }
  if (validator.IsEmpty()) {
    LOG(("SparseRegionReader: region %" PRId64
         " has no strong validator; aborting assembly",
         r.mRegionId));
    Fail();
    return NS_OK;
  }
  if (mIndex == 0) {
    mFirstValidator = validator;
  } else if (!mFirstValidator.Equals(validator)) {
    LOG(
        ("SparseRegionReader: validator mismatch between regions; aborting "
         "assembly"));
    Fail();
    return NS_OK;
  }

  nsCOMPtr<nsIInputStream> base;
  if (NS_FAILED(aEntry->OpenBoundedInputStream(r.mChildOffset,
                                               r.mChildOffset + r.mSliceLen,
                                               getter_AddRefs(base)))) {
    Fail();
    return NS_OK;
  }
  nsCOMPtr<nsIInputStream> sliced =
      new SlicedInputStream(base.forget(), 0, uint64_t(r.mSliceLen));
  mStreams.AppendElement(sliced);
  if (mIndex == 0) {
    mFirstEntry = aEntry;
  }

  if (mIndex + 1 < mRegions.Length()) {
    if (NS_FAILED(OpenRegion(mIndex + 1))) {
      Fail();
    }
    return NS_OK;
  }

  // Every region covered: hand the windowed slice streams back to the
  // channel (which decides how to multiplex/wrap them: plain concatenation
  // for multi-region, MIME-framed multipart for multi-range).
  mDone = true;
  RefPtr<nsHttpChannel> chan = mChannel;
  nsCOMPtr<nsICacheEntry> firstEntry = mFirstEntry;
  nsTArray<nsCOMPtr<nsIInputStream>> streams = std::move(mStreams);
  chan->OnSparseSpanReady(std::move(streams), firstEntry);
  return NS_OK;
}

// --- SparseMetaResolver ---------------------------------------------------

NS_IMPL_ISUPPORTS(SparseMetaResolver, nsICacheEntryOpenCallback)

SparseMetaResolver::SparseMetaResolver(nsHttpChannel* aChannel,
                                       nsICacheStorage* aStorage, nsIURI* aURI,
                                       const nsACString& aIdExtensionBase,
                                       SparseMetaResolveKind aKind,
                                       int64_t aParam)
    : mChannel(aChannel),
      mStorage(aStorage),
      mURI(aURI),
      mIdExtensionBase(aIdExtensionBase),
      mKind(aKind),
      mParam(aParam) {}

nsresult SparseMetaResolver::Start() {
  nsAutoCString ext(mIdExtensionBase);
  SparseMeta::AppendMetaExtension(ext);
  return mStorage->AsyncOpenURI(mURI, ext, nsICacheStorage::OPEN_READONLY,
                                this);
}

NS_IMETHODIMP SparseMetaResolver::OnCacheEntryCheck(nsICacheEntry* aEntry,
                                                    uint32_t* aResult) {
  // Reject expired sentinel entries so a server that later adds a strong
  // validator gets retried instead of being permanently suppressed.
  uint32_t expirationTime = nsICacheEntry::NO_EXPIRATION_TIME;
  (void)aEntry->GetExpirationTime(&expirationTime);
  if (expirationTime != nsICacheEntry::NO_EXPIRATION_TIME) {
    uint32_t now = uint32_t(PR_Now() / PR_USEC_PER_SEC);
    if (expirationTime <= now) {
      *aResult = nsICacheEntryOpenCallback::ENTRY_NOT_WANTED;
      return NS_OK;
    }
  }
  *aResult = nsICacheEntryOpenCallback::ENTRY_WANTED;
  return NS_OK;
}

NS_IMETHODIMP SparseMetaResolver::OnCacheEntryAvailable(nsICacheEntry* aEntry,
                                                        bool aNew,
                                                        nsresult aStatus) {
  if (mDone) {
    return NS_OK;
  }
  mDone = true;

  RefPtr<nsHttpChannel> chan = mChannel;
  if (NS_FAILED(aStatus) || !aEntry || aNew) {
    chan->OnSparseMetaUnresolved();
    return NS_OK;
  }
  SparseMeta::Record record;
  if (!SparseMeta::Read(aEntry, record) || record.mNotSparse ||
      record.mTotal <= 0 || record.mValidator.IsEmpty()) {
    chan->OnSparseMetaUnresolved();
    return NS_OK;
  }

  int64_t start = -1;
  int64_t end = -1;
  if (mKind == SparseMetaResolveKind::Suffix) {
    // bytes=-N → last N bytes of the entity.
    int64_t n = std::min(mParam, record.mTotal);
    start = record.mTotal - n;
    end = record.mTotal;
  } else {
    // bytes=N- → from N to the end. Reject if N is beyond the entity.
    if (mParam >= record.mTotal) {
      chan->OnSparseMetaUnresolved();
      return NS_OK;
    }
    start = mParam;
    end = record.mTotal;
  }
  chan->OnSparseMetaResolved(start, end, record.mTotal, record.mValidator,
                             record.mContentType, record.mResponseHead,
                             record.mRev, mStorage);
  return NS_OK;
}

// --- SparseMetaWriter -----------------------------------------------------

NS_IMPL_ISUPPORTS(SparseMetaWriter, nsICacheEntryOpenCallback)

SparseMetaWriter::SparseMetaWriter(nsICacheStorage* aStorage, nsIURI* aURI,
                                   const nsACString& aIdExtension,
                                   SparseMeta::Record&& aRecord)
    : mStorage(aStorage),
      mURI(aURI),
      mIdExtension(aIdExtension),
      mRecord(std::move(aRecord)) {}

nsresult SparseMetaWriter::Start() {
  return mStorage->AsyncOpenURI(mURI, mIdExtension,
                                nsICacheStorage::OPEN_NORMALLY, this);
}

NS_IMETHODIMP SparseMetaWriter::OnCacheEntryCheck(nsICacheEntry* aEntry,
                                                  uint32_t* aResult) {
  *aResult = nsICacheEntryOpenCallback::ENTRY_WANTED;
  return NS_OK;
}

NS_IMETHODIMP SparseMetaWriter::OnCacheEntryAvailable(nsICacheEntry* aEntry,
                                                      bool aNew,
                                                      nsresult aStatus) {
  if (NS_FAILED(aStatus) || !aEntry) {
    return NS_OK;
  }
  (void)SparseMeta::Write(aEntry, mRecord);
  // Give not-sparse sentinel entries a 24-hour TTL so a server that later
  // adds a strong validator (e.g. CDN config change) is retried rather than
  // permanently suppressed.
  if (mRecord.mNotSparse) {
    uint32_t expiry = uint32_t(PR_Now() / PR_USEC_PER_SEC) + 86400;
    (void)aEntry->SetExpirationTime(expiry);
  }
  (void)aEntry->MetaDataReady();
  return NS_OK;
}

// --- SparseWriteQueue -----------------------------------------------------

nsTHashMap<nsCStringHashKey, SparseWriteQueue::State>& SparseWriteQueue::Map() {
  MOZ_ASSERT(NS_IsMainThread());
  static nsTHashMap<nsCStringHashKey, State> sMap;
  return sMap;
}

bool SparseWriteQueue::TryAcquireOrSkip(const nsACString& aKey) {
  MOZ_ASSERT(NS_IsMainThread());
  State& s = Map().LookupOrInsert(nsCString(aKey));
  if (!s.mHeld) {
    s.mHeld = true;
    return true;
  }
  return false;
}

bool SparseWriteQueue::TryAcquire(const nsACString& aKey, ReadyCb&& aCallback) {
  MOZ_ASSERT(NS_IsMainThread());
  State& s = Map().LookupOrInsert(nsCString(aKey));
  if (!s.mHeld) {
    s.mHeld = true;
    return true;
  }
  s.mWaiters.AppendElement(std::move(aCallback));
  return false;
}

void SparseWriteQueue::Release(const nsACString& aKey) {
  MOZ_ASSERT(NS_IsMainThread());
  auto entry = Map().Lookup(aKey);
  if (!entry) {
    return;
  }
  if (entry->mWaiters.IsEmpty()) {
    entry.Remove();
    return;
  }
  ReadyCb cb = std::move(entry->mWaiters[0]);
  entry->mWaiters.RemoveElementAt(0);
  // mHeld stays true — the dispatched callback inherits the slot.
  NS_DispatchToCurrentThread(
      NS_NewRunnableFunction("SparseWriteQueue::Drain", std::move(cb)));
}

// --- SparseMultiRangePartWriter -------------------------------------------

NS_IMPL_ISUPPORTS(SparseMultiRangePartWriter, nsICacheEntryOpenCallback)

SparseMultiRangePartWriter::SparseMultiRangePartWriter(
    nsICacheStorage* aStorage, nsIURI* aURI, const nsACString& aIdExtensionBase,
    nsHttpResponseHead* aResponseHead, nsTArray<Part>&& aParts)
    : mStorage(aStorage),
      mURI(aURI),
      mIdExtensionBase(aIdExtensionBase),
      mParts(std::move(aParts)) {
  if (aResponseHead) {
    mResponseHead = MakeUnique<nsHttpResponseHead>(*aResponseHead);
  }
}

nsresult SparseMultiRangePartWriter::Start() { return OpenPart(0); }

nsresult SparseMultiRangePartWriter::OpenPart(uint32_t aIndex) {
  if (aIndex >= mParts.Length()) {
    return NS_OK;  // All parts processed.
  }
  mIndex = aIndex;
  // Build the full key once so Release matches Acquire even if mParts moves.
  mCurrentKey.Truncate();
  mCurrentKey.Append(mIdExtensionBase);
  mCurrentKey.AppendLiteral(":sparsechunk=");
  mCurrentKey.AppendInt(mParts[aIndex].mRegionId);

  RefPtr<SparseMultiRangePartWriter> self = this;
  auto dispatchOpen = [self]() {
    nsresult rv = self->mStorage->AsyncOpenURI(self->mURI, self->mCurrentKey,
                                               nsICacheStorage::OPEN_NORMALLY,
                                               self.get());
    if (NS_FAILED(rv)) {
      self->ReleaseAndAdvance();
    }
  };
  if (SparseWriteQueue::TryAcquire(mCurrentKey,
                                   [dispatchOpen]() { dispatchOpen(); })) {
    dispatchOpen();
  }
  return NS_OK;
}

void SparseMultiRangePartWriter::ReleaseAndAdvance() {
  if (!mCurrentKey.IsEmpty()) {
    SparseWriteQueue::Release(mCurrentKey);
    mCurrentKey.Truncate();
  }
  uint32_t next = mIndex + 1;
  if (next < mParts.Length()) {
    (void)OpenPart(next);
  }
}

NS_IMETHODIMP SparseMultiRangePartWriter::OnCacheEntryCheck(
    nsICacheEntry* aEntry, uint32_t* aResult) {
  *aResult = nsICacheEntryOpenCallback::ENTRY_WANTED;
  return NS_OK;
}

NS_IMETHODIMP SparseMultiRangePartWriter::OnCacheEntryAvailable(
    nsICacheEntry* aEntry, bool aNew, nsresult aStatus) {
  if (NS_FAILED(aStatus) || !aEntry) {
    int64_t regionId =
        mIndex < mParts.Length() ? mParts[mIndex].mRegionId : int64_t{-1};
    LOG(("SparseMultiRangePartWriter: AsyncOpenURI failed for region %" PRId64
         " (status=0x%08x aEntry=%p)",
         regionId, static_cast<uint32_t>(aStatus), aEntry));
    ReleaseAndAdvance();
    return NS_OK;
  }

  const Part& part = mParts[mIndex];
  // Open the entry's output stream at this part's child offset and write
  // the part body first (sparse storage marks the range valid via the
  // output stream's write hook). The entry's post-write extent is
  // childOffset + body, not just the body length — pass that as the
  // predicted size so the cache's per-entry size limiter sees the right
  // number.
  nsCOMPtr<nsIOutputStream> out;
  nsresult rv = aEntry->OpenOutputStream(
      part.mChildOffset, part.mChildOffset + part.mBody.Length(),
      getter_AddRefs(out));
  if (NS_FAILED(rv) || !out) {
    LOG(
        ("SparseMultiRangePartWriter: OpenOutputStream failed (%08x) for "
         "region %" PRId64,
         static_cast<uint32_t>(rv), part.mRegionId));
    ReleaseAndAdvance();
    return NS_OK;
  }
  uint32_t written = 0;
  rv = out->Write(part.mBody.BeginReading(), part.mBody.Length(), &written);
  (void)out->Close();
  if (NS_FAILED(rv) || written != part.mBody.Length()) {
    LOG(("SparseMultiRangePartWriter: short/failed write for region %" PRId64,
         part.mRegionId));
    ReleaseAndAdvance();
    return NS_OK;
  }
#ifndef ANDROID
  glean::network::byte_range_request.Get("sparse_write_part_succeeded"_ns)
      .Add(1);
#endif

  // After the data is committed, store a synthetic 206 response head so a
  // later OnCacheEntryCheckSparse has the validator / total / content-type
  // it needs to serve.
  if (mResponseHead) {
    auto head = MakeUnique<nsHttpResponseHead>(*mResponseHead);
    (void)head->ParseStatusLine("HTTP/1.1 206 Partial Content"_ns);
    nsAutoCString contentRange;
    contentRange.AppendLiteral("bytes ");
    contentRange.AppendInt(part.mFirst);
    contentRange.Append('-');
    contentRange.AppendInt(part.mLast);
    contentRange.Append('/');
    if (part.mTotal < 0) {
      contentRange.Append('*');
    } else {
      contentRange.AppendInt(part.mTotal);
    }
    (void)head->SetHeader(nsHttp::Content_Range, contentRange);
    head->SetContentLength(part.mBody.Length());
    // Replace the multipart content-type with a generic one for the stored
    // region head; the part's actual Content-Type isn't carried in the Part
    // struct, and a generic type is sufficient for serve to construct a 206.
    (void)head->SetHeader(nsHttp::Content_Type, "application/octet-stream"_ns);
    nsAutoCString flat;
    head->Flatten(flat, true);
    (void)aEntry->SetMetaDataElement("response-head", flat.get());
    (void)aEntry->SetMetaDataElement("request-method", "GET");

    // Stamp the region with the URL-level strong validator (mirrored on
    // the per-URL :sparsemeta entry) so SparseRegionReader can compare
    // validators across regions cheaply without parsing each region's
    // full stored head.
    nsAutoCString validator;
    SparseMeta::ValidatorKind kind = SparseMeta::ValidatorKind::None;
    if (SparseMeta::ExtractValidator(mResponseHead.get(), validator, kind)) {
      (void)SparseMeta::StampRegion(aEntry, validator, kind);
    }
  }
  (void)aEntry->MetaDataReady();
  ReleaseAndAdvance();
  return NS_OK;
}

// --- SparseMultiRangeCaptureListener --------------------------------------

NS_IMPL_ISUPPORTS(SparseMultiRangeCaptureListener, nsIStreamListener,
                  nsIRequestObserver)

SparseMultiRangeCaptureListener::SparseMultiRangeCaptureListener(
    nsHttpChannel* aChannel, nsIStreamListener* aConsumer,
    const nsACString& aContentType)
    : mChannel(aChannel), mConsumer(aConsumer), mContentType(aContentType) {}

NS_IMETHODIMP SparseMultiRangeCaptureListener::OnStartRequest(
    nsIRequest* aReq) {
  return mConsumer->OnStartRequest(aReq);
}

NS_IMETHODIMP SparseMultiRangeCaptureListener::OnDataAvailable(
    nsIRequest* aReq, nsIInputStream* aStream, uint64_t aOffset,
    uint32_t aCount) {
  // Read the chunk to a local buffer so we can both capture it and forward
  // to the consumer (which can't re-read aStream once we've consumed it).
  nsAutoCString chunk;
  if (!chunk.SetLength(aCount, fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  uint32_t read = 0;
  nsresult rv = aStream->Read(chunk.BeginWriting(), aCount, &read);
  if (NS_FAILED(rv)) {
    return rv;
  }
  chunk.SetLength(read);

  if (!mCappedOut) {
    if (mBuffer.Length() + read > kMaxBuffer ||
        !mBuffer.Append(chunk, fallible)) {
      mCappedOut = true;
      mBuffer.Truncate();
#ifndef ANDROID
      glean::network::byte_range_request.Get("sparse_capture_oversize"_ns)
          .Add(1);
#endif
    }
  }

  nsCOMPtr<nsIInputStream> tempStream;
  rv = NS_NewCStringInputStream(getter_AddRefs(tempStream), chunk);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return mConsumer->OnDataAvailable(aReq, tempStream, aOffset, read);
}

NS_IMETHODIMP SparseMultiRangeCaptureListener::OnStopRequest(nsIRequest* aReq,
                                                             nsresult aStatus) {
  nsresult rv = mConsumer->OnStopRequest(aReq, aStatus);
  if (NS_SUCCEEDED(aStatus) && !mCappedOut && !mBuffer.IsEmpty()) {
    mChannel->OnSparseMultiRangeBodyCaptured(std::move(mBuffer), mContentType);
  }
  return rv;
}

// --- SparseSingleRangeCaptureListener -------------------------------------

NS_IMPL_ISUPPORTS(SparseSingleRangeCaptureListener, nsIStreamListener,
                  nsIRequestObserver)

SparseSingleRangeCaptureListener::SparseSingleRangeCaptureListener(
    nsHttpChannel* aChannel, nsIStreamListener* aConsumer)
    : mChannel(aChannel), mConsumer(aConsumer) {}

NS_IMETHODIMP SparseSingleRangeCaptureListener::OnStartRequest(
    nsIRequest* aReq) {
  return mConsumer->OnStartRequest(aReq);
}

NS_IMETHODIMP SparseSingleRangeCaptureListener::OnDataAvailable(
    nsIRequest* aReq, nsIInputStream* aStream, uint64_t aOffset,
    uint32_t aCount) {
  nsAutoCString chunk;
  if (!chunk.SetLength(aCount, fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  uint32_t read = 0;
  nsresult rv = aStream->Read(chunk.BeginWriting(), aCount, &read);
  if (NS_FAILED(rv)) {
    return rv;
  }
  chunk.SetLength(read);

  if (!mCappedOut) {
    if (mBuffer.Length() + read > kMaxBuffer ||
        !mBuffer.Append(chunk, fallible)) {
      mCappedOut = true;
      mBuffer.Truncate();
#ifndef ANDROID
      glean::network::byte_range_request.Get("sparse_capture_oversize"_ns)
          .Add(1);
#endif
    }
  }

  nsCOMPtr<nsIInputStream> tempStream;
  rv = NS_NewCStringInputStream(getter_AddRefs(tempStream), chunk);
  if (NS_FAILED(rv)) {
    return rv;
  }
  return mConsumer->OnDataAvailable(aReq, tempStream, aOffset, read);
}

NS_IMETHODIMP SparseSingleRangeCaptureListener::OnStopRequest(
    nsIRequest* aReq, nsresult aStatus) {
  nsresult rv = mConsumer->OnStopRequest(aReq, aStatus);
  if (NS_SUCCEEDED(aStatus) && !mCappedOut && !mBuffer.IsEmpty()) {
    mChannel->OnSparseSingleRangeBodyCaptured(std::move(mBuffer));
  }
  return rv;
}

// --- Free functions -------------------------------------------------------

void GenerateBoundary(nsACString& aBoundary) {
  static Atomic<uint32_t> sCounter{0};
  uint32_t n = ++sCounter;
  aBoundary.AssignLiteral("ffsparse_");
  aBoundary.AppendInt(static_cast<int64_t>(PR_Now() / PR_USEC_PER_SEC));
  aBoundary.Append('_');
  aBoundary.AppendInt(n);
}

void PushRegionSlices(nsTArray<SparseMultiRangePartWriter::Part>& aOut,
                      const nsACString& aBody, int64_t aFirst, int64_t aLast,
                      int64_t aTotal, int64_t aChunkSize) {
  if (aFirst < 0 || aLast < aFirst || aChunkSize <= 0) {
    return;
  }
  int64_t expected = aLast - aFirst + 1;
  if (int64_t(aBody.Length()) != expected) {
    return;
  }
  int64_t pos = aFirst;
  size_t bodyPos = 0;
  while (pos <= aLast) {
    int64_t region = pos / aChunkSize;
    int64_t regionEnd = (region + 1) * aChunkSize;
    int64_t sliceFirst = pos;
    int64_t sliceLast = std::min<int64_t>(aLast, regionEnd - 1);
    int64_t sliceLen = sliceLast - sliceFirst + 1;
    SparseMultiRangePartWriter::Part part;
    part.mRegionId = region;
    part.mChildOffset = sliceFirst - region * aChunkSize;
    part.mFirst = sliceFirst;
    part.mLast = sliceLast;
    part.mTotal = aTotal;
    part.mBody.Assign(Substring(aBody, bodyPos, sliceLen));
    aOut.AppendElement(std::move(part));
    pos = sliceLast + 1;
    bodyPos += sliceLen;
  }
}

bool ParsePartHeaders(const nsCString& aBody, size_t* aPos, int64_t* aFirst,
                      int64_t* aLast, int64_t* aTotal) {
  bool foundRange = false;
  size_t p = *aPos;
  while (p < aBody.Length()) {
    auto eol = aBody.Find("\r\n", p);
    if (eol == kNotFound) {
      return false;
    }
    if (size_t(eol) == p) {
      // Blank line: end of headers.
      *aPos = p + 2;
      return foundRange;
    }
    nsDependentCSubstring line(aBody, p, eol - p);
    auto colon = line.FindChar(':');
    if (colon != kNotFound) {
      nsAutoCString name(Substring(line, 0, colon));
      ToLowerCase(name);
      if (name.EqualsLiteral("content-range")) {
        nsDependentCSubstring value(line, colon + 1);
        nsAutoCString trimmed(value);
        trimmed.Trim(" \t");
        if (nsHttp::ParseContentRangeHeader(trimmed, aFirst, aLast, aTotal)) {
          foundRange = true;
        }
      }
    }
    p = eol + 2;
  }
  return false;
}

bool ParseSingleSuffixOrOpenEndedByteRange(const nsACString& aValue,
                                           SparseMetaResolveKind* aOutKind,
                                           int64_t* aOutParam) {
  nsAutoCString value(aValue);
  value.StripWhitespace();
  if (value.Length() < 7) {
    return false;
  }
  if (!Substring(value, 0, 6).LowerCaseEqualsLiteral("bytes=")) {
    return false;
  }
  const nsACString& spec = Substring(value, 6);
  if (spec.Contains(',')) {
    return false;
  }
  int32_t dash = spec.FindChar('-');
  if (dash < 0) {
    return false;
  }
  if (dash == 0) {
    if (spec.Length() < 2) {
      return false;
    }
    for (uint32_t i = 1; i < spec.Length(); ++i) {
      if (spec[i] < '0' || spec[i] > '9') {
        return false;
      }
    }
    nsCString num(Substring(spec, 1));
    nsresult rv = NS_OK;
    int64_t n = num.ToInteger64(&rv);
    if (NS_FAILED(rv) || n <= 0) {
      return false;
    }
    *aOutKind = SparseMetaResolveKind::Suffix;
    *aOutParam = n;
    return true;
  }
  if (uint32_t(dash) != spec.Length() - 1) {
    return false;
  }
  for (int32_t i = 0; i < dash; ++i) {
    if (spec[i] < '0' || spec[i] > '9') {
      return false;
    }
  }
  nsCString num(Substring(spec, 0, dash));
  nsresult rv = NS_OK;
  int64_t n = num.ToInteger64(&rv);
  if (NS_FAILED(rv) || n < 0) {
    return false;
  }
  *aOutKind = SparseMetaResolveKind::OpenEnded;
  *aOutParam = n;
  return true;
}

bool IsSingleSuffixOrOpenEndedByteRange(const nsACString& aValue) {
  SparseMetaResolveKind kind;
  int64_t param;
  return ParseSingleSuffixOrOpenEndedByteRange(aValue, &kind, &param);
}

}  // namespace mozilla::net
