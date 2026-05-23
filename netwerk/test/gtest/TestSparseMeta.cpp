/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "SparseMeta.h"
#include "nsHttpResponseHead.h"
#include "nsICacheEntry.h"
#include "nsString.h"
#include <map>
#include <string>

using namespace mozilla::net;
using mozilla::net::SparseMeta::Record;
using mozilla::net::SparseMeta::ValidatorKind;

namespace {

// Minimal nsICacheEntry stand-in: implements the metadata-element accessors
// SparseMeta touches; everything else returns NS_ERROR_NOT_IMPLEMENTED.
class MockCacheEntry final : public nsICacheEntry {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSICACHEENTRY

  std::map<std::string, std::string> mMeta;

 private:
  ~MockCacheEntry() = default;
};

NS_IMPL_ISUPPORTS(MockCacheEntry, nsICacheEntry)

// Only these two are actually wired to the in-memory map.
NS_IMETHODIMP MockCacheEntry::GetMetaDataElement(const char* aKey,
                                                 char** aValue) {
  auto it = mMeta.find(std::string(aKey));
  if (it == mMeta.end()) {
    *aValue = nullptr;
    return NS_ERROR_NOT_AVAILABLE;
  }
  *aValue = NS_xstrdup(it->second.c_str());
  return NS_OK;
}
NS_IMETHODIMP MockCacheEntry::SetMetaDataElement(const char* aKey,
                                                 const char* aValue) {
  if (!aValue) {
    mMeta.erase(std::string(aKey));
    return NS_OK;
  }
  mMeta[std::string(aKey)] = std::string(aValue);
  return NS_OK;
}

// The rest are unused by SparseMeta — return NS_ERROR_NOT_IMPLEMENTED.
#define SPARSEMETA_TEST_NOT_IMPLEMENTED \
  {                                     \
    return NS_ERROR_NOT_IMPLEMENTED;    \
  }
NS_IMETHODIMP MockCacheEntry::GetKey(nsACString&)
    SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetCacheEntryId(uint64_t*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetPersistent(bool*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetReadyOrRevalidating(bool*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetFetchCount(uint32_t*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetLastFetched(uint32_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetLastModified(uint32_t*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetExpirationTime(uint32_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::SetExpirationTime(uint32_t) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetOnStartTime(uint64_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetOnStopTime(uint64_t*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::SetNetworkTimes(uint64_t, uint64_t)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::SetContentType(uint8_t) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::ForceValidFor(uint32_t)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetIsForcedValid(bool*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::MarkForcedValidUse()
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::OpenInputStream(int64_t, nsIInputStream**)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::OpenBoundedInputStream(int64_t, int64_t, nsIInputStream**)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::OpenOutputStream(int64_t, int64_t, nsIOutputStream**)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetSecurityInfo(nsITransportSecurityInfo**)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::SetSecurityInfo(nsITransportSecurityInfo*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetStorageDataSize(uint32_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::AsyncDoom(nsICacheEntryDoomCallback*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetIsEmpty(bool*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::VisitMetaData(nsICacheEntryMetaDataVisitor*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::MetaDataReady() SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::SetValid() SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::Dismiss() SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetDiskStorageSizeInKB(uint32_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP MockCacheEntry::Recreate(
            bool, nsICacheEntry**) SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetDataSize(int64_t*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::IsRangeCached(int64_t, int64_t, bool*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::FirstAvailableRange(int64_t, int64_t*, int64_t*,
                                        bool*) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetAltDataSize(int64_t*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::GetAltDataType(nsACString&) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::OpenAlternativeOutputStream(
        const nsACString&, int64_t,
        nsIAsyncOutputStream**) SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::OpenAlternativeInputStream(
        const nsACString&, nsIInputStream**) SPARSEMETA_TEST_NOT_IMPLEMENTED
    NS_IMETHODIMP MockCacheEntry::GetLoadContextInfo(nsILoadContextInfo**)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::SetDictionary(mozilla::net::DictionaryCacheEntry*)
        SPARSEMETA_TEST_NOT_IMPLEMENTED NS_IMETHODIMP
    MockCacheEntry::SetBypassWriterLock(bool) SPARSEMETA_TEST_NOT_IMPLEMENTED
#undef SPARSEMETA_TEST_NOT_IMPLEMENTED

    Record
    MakeRecord(int64_t aTotal, const char* aValidator, ValidatorKind aKind) {
  Record r;
  r.mTotal = aTotal;
  r.mValidator.Assign(aValidator);
  r.mKind = aKind;
  r.mContentType.AssignLiteral("application/octet-stream");
  r.mContentEncoding.Truncate();
  r.mResponseHead.Truncate();
  return r;
}

}  // namespace

TEST(TestSparseMeta, ValidatorKindRoundTrip)
{
  EXPECT_STREQ("etag", SparseMeta::ValidatorKindToString(ValidatorKind::ETag));
  EXPECT_STREQ("lastmod",
               SparseMeta::ValidatorKindToString(ValidatorKind::LastModified));
  EXPECT_STREQ("", SparseMeta::ValidatorKindToString(ValidatorKind::None));

  EXPECT_EQ(ValidatorKind::ETag,
            SparseMeta::ValidatorKindFromString("etag"_ns));
  EXPECT_EQ(ValidatorKind::LastModified,
            SparseMeta::ValidatorKindFromString("lastmod"_ns));
  EXPECT_EQ(ValidatorKind::None, SparseMeta::ValidatorKindFromString(""_ns));
  EXPECT_EQ(ValidatorKind::None,
            SparseMeta::ValidatorKindFromString("garbage"_ns));
}

TEST(TestSparseMeta, AppendMetaExtension)
{
  nsAutoCString ext("base");
  SparseMeta::AppendMetaExtension(ext);
  EXPECT_STREQ("base:sparsemeta", ext.get());
}

TEST(TestSparseMeta, ExtractValidatorPrefersStrongETag)
{
  auto head = mozilla::MakeUnique<nsHttpResponseHead>();
  (void)head->ParseStatusLine("HTTP/1.1 206 Partial Content"_ns);
  (void)head->SetHeader(nsHttp::ETag, "\"abc\""_ns);
  (void)head->SetHeader(nsHttp::Last_Modified,
                        "Wed, 21 Oct 2015 07:28:00 GMT"_ns);

  nsAutoCString v;
  ValidatorKind k = ValidatorKind::None;
  EXPECT_TRUE(SparseMeta::ExtractValidator(head.get(), v, k));
  EXPECT_EQ(ValidatorKind::ETag, k);
  EXPECT_STREQ("\"abc\"", v.get());
}

TEST(TestSparseMeta, ExtractValidatorRejectsWeakETag)
{
  auto head = mozilla::MakeUnique<nsHttpResponseHead>();
  (void)head->ParseStatusLine("HTTP/1.1 206 Partial Content"_ns);
  (void)head->SetHeader(nsHttp::ETag, "W/\"abc\""_ns);
  (void)head->SetHeader(nsHttp::Last_Modified,
                        "Wed, 21 Oct 2015 07:28:00 GMT"_ns);

  nsAutoCString v;
  ValidatorKind k = ValidatorKind::None;
  EXPECT_TRUE(SparseMeta::ExtractValidator(head.get(), v, k));
  EXPECT_EQ(ValidatorKind::LastModified, k);
  EXPECT_STREQ("Wed, 21 Oct 2015 07:28:00 GMT", v.get());
}

TEST(TestSparseMeta, ExtractValidatorNoStrongValidator)
{
  auto head = mozilla::MakeUnique<nsHttpResponseHead>();
  (void)head->ParseStatusLine("HTTP/1.1 206 Partial Content"_ns);
  (void)head->SetHeader(nsHttp::ETag, "W/\"abc\""_ns);  // weak only

  nsAutoCString v;
  ValidatorKind k = ValidatorKind::None;
  EXPECT_FALSE(SparseMeta::ExtractValidator(head.get(), v, k));
  EXPECT_EQ(ValidatorKind::None, k);
  EXPECT_TRUE(v.IsEmpty());
}

TEST(TestSparseMeta, RoundTrip)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  Record in = MakeRecord(65043116, "\"abc\"", ValidatorKind::ETag);
  in.mResponseHead.AssignLiteral("HTTP/1.1 206 Partial Content\r\n");

  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, in));

  Record out;
  EXPECT_TRUE(SparseMeta::Read(entry, out));
  EXPECT_EQ(in.mTotal, out.mTotal);
  EXPECT_TRUE(out.mValidator.Equals(in.mValidator));
  EXPECT_EQ(ValidatorKind::ETag, out.mKind);
  EXPECT_TRUE(out.mContentType.Equals(in.mContentType));
  EXPECT_FALSE(out.mNotSparse);
  EXPECT_EQ(1u, out.mRev);  // fresh write -> rev=1
  EXPECT_FALSE(out.mResponseHead.IsEmpty());
}

TEST(TestSparseMeta, RevPreservedOnValidatorMatch)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  Record r1 = MakeRecord(1000, "\"v1\"", ValidatorKind::ETag);
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, r1));

  Record after1;
  ASSERT_TRUE(SparseMeta::Read(entry, after1));
  EXPECT_EQ(1u, after1.mRev);

  Record r2 = MakeRecord(1000, "\"v1\"", ValidatorKind::ETag);
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, r2));

  Record after2;
  ASSERT_TRUE(SparseMeta::Read(entry, after2));
  EXPECT_EQ(1u, after2.mRev);  // matched validator -> rev unchanged
}

TEST(TestSparseMeta, RevBumpedOnValidatorChange)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  Record r1 = MakeRecord(1000, "\"v1\"", ValidatorKind::ETag);
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, r1));

  Record r2 = MakeRecord(1000, "\"v2\"", ValidatorKind::ETag);
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, r2));

  Record after;
  ASSERT_TRUE(SparseMeta::Read(entry, after));
  EXPECT_EQ(2u, after.mRev);
  EXPECT_TRUE(after.mValidator.EqualsLiteral("\"v2\""));
}

TEST(TestSparseMeta, NotSparseSentinelRoundTrip)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  Record sentinel;
  sentinel.mNotSparse = true;
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, sentinel));

  Record out;
  EXPECT_TRUE(SparseMeta::Read(entry, out));
  EXPECT_TRUE(out.mNotSparse);
  EXPECT_EQ(-1, out.mTotal);  // sentinel reads stop before populating total
  EXPECT_TRUE(out.mValidator.IsEmpty());
}

TEST(TestSparseMeta, SentinelOverwrittenByRealRecord)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  Record sentinel;
  sentinel.mNotSparse = true;
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, sentinel));

  Record real = MakeRecord(500, "\"x\"", ValidatorKind::ETag);
  EXPECT_EQ(NS_OK, SparseMeta::Write(entry, real));

  Record out;
  EXPECT_TRUE(SparseMeta::Read(entry, out));
  EXPECT_FALSE(out.mNotSparse);
  EXPECT_EQ(500, out.mTotal);
}

TEST(TestSparseMeta, ReadFailsWithoutVersion)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  // No write — entry is empty.
  Record out;
  EXPECT_FALSE(SparseMeta::Read(entry, out));
}

TEST(TestSparseMeta, ReadFailsOnUnknownVersion)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  (void)entry->SetMetaDataElement(SparseMeta::kVersion, "99");
  (void)entry->SetMetaDataElement(SparseMeta::kTotal, "1000");
  (void)entry->SetMetaDataElement(SparseMeta::kValidator, "\"x\"");
  (void)entry->SetMetaDataElement(SparseMeta::kValidatorKind, "etag");

  Record out;
  EXPECT_FALSE(SparseMeta::Read(entry, out));
}

TEST(TestSparseMeta, StampRegionIsIdempotent)
{
  RefPtr<MockCacheEntry> entry = new MockCacheEntry();
  EXPECT_EQ(NS_OK,
            SparseMeta::StampRegion(entry, "\"v1\""_ns, ValidatorKind::ETag));
  EXPECT_EQ(NS_OK,
            SparseMeta::StampRegion(entry, "\"v1\""_ns, ValidatorKind::ETag));

  nsCString v, k;
  (void)entry->GetMetaDataElement(SparseMeta::kRegionValidator,
                                  getter_Copies(v));
  (void)entry->GetMetaDataElement(SparseMeta::kRegionValidatorKind,
                                  getter_Copies(k));
  EXPECT_STREQ("\"v1\"", v.get());
  EXPECT_STREQ("etag", k.get());
}
