/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SparseMeta.h"

#include "nsHttp.h"
#include "nsHttpResponseHead.h"
#include "nsICacheEntry.h"
#include "nsPrintfCString.h"

namespace mozilla::net::SparseMeta {

void AppendMetaExtension(nsACString& aExt) {
  aExt.AppendLiteral(":sparsemeta");
}

const char* ValidatorKindToString(ValidatorKind aKind) {
  switch (aKind) {
    case ValidatorKind::ETag:
      return "etag";
    case ValidatorKind::LastModified:
      return "lastmod";
    case ValidatorKind::None:
    default:
      return "";
  }
}

ValidatorKind ValidatorKindFromString(const nsACString& aString) {
  if (aString.EqualsLiteral("etag")) {
    return ValidatorKind::ETag;
  }
  if (aString.EqualsLiteral("lastmod")) {
    return ValidatorKind::LastModified;
  }
  return ValidatorKind::None;
}

static void GetMetaString(nsICacheEntry* aEntry, const char* aKey,
                          nsACString& aOut) {
  aOut.Truncate();
  nsCString value;
  // getter_Copies' temporary destructor runs Adopt(); split into its own
  // statement so the next read of `value` sees the populated data.
  nsresult rv = aEntry->GetMetaDataElement(aKey, getter_Copies(value));
  if (NS_FAILED(rv)) {
    return;
  }
  aOut = value;
}

bool Read(nsICacheEntry* aEntry, Record& aOut) {
  if (!aEntry) {
    return false;
  }

  // The nsTGetterCopies temporary's destructor performs the Adopt that
  // populates the string; it doesn't run until the end of the full
  // expression that *contains* the temporary. Splitting the GetMetaDataElement
  // call into its own statement guarantees the destructor has fired before
  // we test the resulting string. The same pattern applies below.
  nsCString version;
  nsresult vrv = aEntry->GetMetaDataElement(kVersion, getter_Copies(version));
  if (NS_FAILED(vrv) || version.IsEmpty()) {
    return false;
  }
  if (!version.EqualsASCII(kCurrentVersion)) {
    // Future schema version we don't understand. Treat as no meta; a
    // subsequent write-through will overwrite to the current schema.
    return false;
  }

  aOut = Record{};

  nsCString notSparse;
  nsresult nrv =
      aEntry->GetMetaDataElement(kNotSparse, getter_Copies(notSparse));
  if (NS_SUCCEEDED(nrv) && notSparse.EqualsLiteral("1")) {
    aOut.mNotSparse = true;
    // Sentinel entries skip the rest of the fields; the caller only needs
    // mNotSparse to short-circuit.
    return true;
  }

  nsCString total;
  GetMetaString(aEntry, kTotal, total);
  if (total.IsEmpty()) {
    return false;
  }
  nsresult rv = NS_OK;
  aOut.mTotal = total.ToInteger64(&rv);
  if (NS_FAILED(rv) || aOut.mTotal <= 0) {
    return false;
  }

  GetMetaString(aEntry, kValidator, aOut.mValidator);
  nsCString kindStr;
  GetMetaString(aEntry, kValidatorKind, kindStr);
  aOut.mKind = ValidatorKindFromString(kindStr);
  if (aOut.mValidator.IsEmpty() || aOut.mKind == ValidatorKind::None) {
    return false;
  }

  GetMetaString(aEntry, kContentType, aOut.mContentType);
  GetMetaString(aEntry, kContentEncoding, aOut.mContentEncoding);

  nsCString rev;
  GetMetaString(aEntry, kRev, rev);
  if (!rev.IsEmpty()) {
    rv = NS_OK;
    aOut.mRev = rev.ToInteger64(&rv);
    if (NS_FAILED(rv)) {
      aOut.mRev = 0;
    }
  }

  GetMetaString(aEntry, "response-head", aOut.mResponseHead);

  return true;
}

nsresult Write(nsICacheEntry* aEntry, const Record& aRecord) {
  if (!aEntry) {
    return NS_ERROR_INVALID_POINTER;
  }

  // Stamp request-method so the cache backend treats this as a GET cache
  // entry. Matches what the existing region writers store.
  nsresult rv = aEntry->SetMetaDataElement("request-method", "GET");
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = aEntry->SetMetaDataElement(kVersion, kCurrentVersion);
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (aRecord.mNotSparse) {
    // Sentinel form: just the flag, clear everything else.
    rv = aEntry->SetMetaDataElement(kNotSparse, "1");
    if (NS_FAILED(rv)) {
      return rv;
    }
    (void)aEntry->SetMetaDataElement(kTotal, nullptr);
    (void)aEntry->SetMetaDataElement(kValidator, nullptr);
    (void)aEntry->SetMetaDataElement(kValidatorKind, nullptr);
    (void)aEntry->SetMetaDataElement(kContentType, nullptr);
    (void)aEntry->SetMetaDataElement(kContentEncoding, nullptr);
    (void)aEntry->SetMetaDataElement("response-head", nullptr);
    return NS_OK;
  }

  // Compare new validator against any existing one to decide whether to
  // preserve or bump the rev counter.
  uint64_t rev = aRecord.mRev;
  nsCString existingValidator;
  GetMetaString(aEntry, kValidator, existingValidator);
  if (existingValidator.IsEmpty()) {
    // Fresh meta: start rev at 1 unless caller already supplied a value.
    if (rev == 0) {
      rev = 1;
    }
  } else if (existingValidator.Equals(aRecord.mValidator)) {
    // Same resource version. Preserve the existing rev.
    nsCString existingRev;
    GetMetaString(aEntry, kRev, existingRev);
    nsresult parseRv = NS_OK;
    uint64_t parsed = existingRev.ToInteger64(&parseRv);
    if (NS_SUCCEEDED(parseRv) && parsed > 0) {
      rev = parsed;
    }
  } else {
    // Validator changed: bump the rev from whatever's stored.
    nsCString existingRev;
    GetMetaString(aEntry, kRev, existingRev);
    nsresult parseRv = NS_OK;
    uint64_t parsed = existingRev.ToInteger64(&parseRv);
    if (NS_SUCCEEDED(parseRv)) {
      rev = parsed + 1;
    } else {
      rev = 1;
    }
  }

  // Clear any stale sentinel from a prior life.
  (void)aEntry->SetMetaDataElement(kNotSparse, nullptr);

  nsPrintfCString totalStr("%" PRId64, aRecord.mTotal);
  rv = aEntry->SetMetaDataElement(kTotal, totalStr.get());
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = aEntry->SetMetaDataElement(kValidator,
                                  PromiseFlatCString(aRecord.mValidator).get());
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = aEntry->SetMetaDataElement(kValidatorKind,
                                  ValidatorKindToString(aRecord.mKind));
  if (NS_FAILED(rv)) {
    return rv;
  }

  rv = aEntry->SetMetaDataElement(
      kContentType, PromiseFlatCString(aRecord.mContentType).get());
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = aEntry->SetMetaDataElement(
      kContentEncoding, PromiseFlatCString(aRecord.mContentEncoding).get());
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsPrintfCString revStr("%" PRIu64, rev);
  rv = aEntry->SetMetaDataElement(kRev, revStr.get());
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (!aRecord.mResponseHead.IsEmpty()) {
    rv = aEntry->SetMetaDataElement(
        "response-head", PromiseFlatCString(aRecord.mResponseHead).get());
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  return NS_OK;
}

nsresult StampRegion(nsICacheEntry* aRegionEntry, const nsACString& aValidator,
                     ValidatorKind aKind) {
  if (!aRegionEntry) {
    return NS_ERROR_INVALID_POINTER;
  }
  nsresult rv = aRegionEntry->SetMetaDataElement(
      kRegionValidator, PromiseFlatCString(aValidator).get());
  if (NS_FAILED(rv)) {
    return rv;
  }
  return aRegionEntry->SetMetaDataElement(kRegionValidatorKind,
                                          ValidatorKindToString(aKind));
}

bool ExtractValidator(nsHttpResponseHead* aHead, nsACString& aValidator,
                      ValidatorKind& aKind) {
  aValidator.Truncate();
  aKind = ValidatorKind::None;
  if (!aHead) {
    return false;
  }

  nsAutoCString etag;
  if (NS_SUCCEEDED(aHead->GetHeader(nsHttp::ETag, etag))) {
    nsAutoCString trimmed(etag);
    trimmed.Trim(" \t");
    if (!trimmed.IsEmpty() && !StringBeginsWith(trimmed, "W/"_ns)) {
      aValidator = trimmed;
      aKind = ValidatorKind::ETag;
      return true;
    }
  }

  nsAutoCString lm;
  if (NS_SUCCEEDED(aHead->GetHeader(nsHttp::Last_Modified, lm)) &&
      !lm.IsEmpty()) {
    aValidator = lm;
    aKind = ValidatorKind::LastModified;
    return true;
  }

  return false;
}

}  // namespace mozilla::net::SparseMeta
