/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_SparseMeta_h_
#define mozilla_net_SparseMeta_h_

// Per-URL cache metadata for the sparse byte-range cache.
//
// Region entries (URL :sparsechunk=<id>) hold the bytes for one chunk-size
// region of a resource; this module manages a sibling URL :sparsemeta entry
// that records the URL-level facts the regions cannot cheaply share — the
// entity total, the strong validator, content-type, and a "this URL is not
// sparse-cacheable" sentinel. Suffix and open-ended sub-range requests use
// it to map to absolute bytes at request time (so warm caches can serve
// suffix requests without a network round-trip); multi-region serves use it
// to skip per-region head reads.
//
// The meta entry has no body; everything lives in nsICacheEntry metadata
// elements. Identical metadata names (sparse-validator, sparse-validator-
// kind) are also stamped onto each region entry by the write-through paths
// so SparseRegionReader can read a small element rather than parsing a full
// stored response head.

#include "nsString.h"

class nsICacheEntry;

namespace mozilla::net {

class nsHttpResponseHead;

namespace SparseMeta {

// nsICacheEntry metadata-element keys for the URL :sparsemeta entry.
inline constexpr const char* kVersion = "sparsemeta-version";
inline constexpr const char* kTotal = "sparsemeta-total";
inline constexpr const char* kValidator = "sparsemeta-validator";
inline constexpr const char* kValidatorKind = "sparsemeta-validator-kind";
inline constexpr const char* kContentType = "sparsemeta-content-type";
inline constexpr const char* kContentEncoding = "sparsemeta-content-encoding";
inline constexpr const char* kRev = "sparsemeta-rev";
inline constexpr const char* kNotSparse = "sparsemeta-not-sparse";

// Region-side mirror (set on each URL :sparsechunk=<id> entry) so the
// multi-region serve coordinator can cheaply confirm all spanned regions
// share a validator without parsing each region's full stored head.
inline constexpr const char* kRegionValidator = "sparse-validator";
inline constexpr const char* kRegionValidatorKind = "sparse-validator-kind";

// Schema version this build writes. Persisted via kVersion; readers compare
// and treat differing versions as "treat meta as missing".
inline constexpr const char* kCurrentVersion = "1";

enum class ValidatorKind : uint8_t {
  None,
  ETag,
  LastModified,
};

struct Record {
  int64_t mTotal = -1;
  nsCString mValidator;
  ValidatorKind mKind = ValidatorKind::None;
  nsCString mContentType;
  nsCString mContentEncoding;
  uint64_t mRev = 0;
  bool mNotSparse = false;
  // Flattened whole-entity 206 head, normalized so Content-Range is
  // bytes 0-<total-1>/<total>. Empty when the entry is a "not-sparse"
  // sentinel or hasn't been populated with a head yet.
  nsCString mResponseHead;
};

// Appends the metadata key suffix ":sparsemeta" to aExt. Callers should
// pass an extension that already includes the base extension from
// nsHttpChannel::AppendBaseCacheIdExtension (POST id / TRR / HEAD / FETCH)
// so the meta entry shares the channel's cache namespace.
void AppendMetaExtension(nsACString& aExt);

// Reads a Record from an already-opened cache entry. Returns true on
// success; false if the version is missing/unknown (treat as no meta).
bool Read(nsICacheEntry* aEntry, Record& aOut);

// Writes a Record to an opened cache entry. Handles signature/rev
// management: if the entry has a prior validator that matches aRecord's,
// rev is preserved; if it differs, rev is bumped. The caller is
// responsible for calling MetaDataReady() once it has staged whatever
// other state the entry needs.
nsresult Write(nsICacheEntry* aEntry, const Record& aRecord);

// Stamps a region entry with the validator + kind so SparseRegionReader
// can compare validators cheaply without parsing the stored response
// head. Idempotent. Caller still calls MetaDataReady().
nsresult StampRegion(nsICacheEntry* aRegionEntry, const nsACString& aValidator,
                     ValidatorKind aKind);

// Extracts a strong validator from a response head. Prefers a strong
// ETag (rejects W/"..." weak ETags); falls back to Last-Modified.
// Returns true with *aKind/aValidator populated when a usable validator
// is present.
bool ExtractValidator(nsHttpResponseHead* aHead, nsACString& aValidator,
                      ValidatorKind& aKind);

// Maps the enum to a stable persistent string and back.
const char* ValidatorKindToString(ValidatorKind aKind);
ValidatorKind ValidatorKindFromString(const nsACString& aString);

}  // namespace SparseMeta
}  // namespace mozilla::net

#endif  // mozilla_net_SparseMeta_h_
