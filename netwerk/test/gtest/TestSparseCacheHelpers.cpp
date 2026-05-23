/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "SparseCache.h"

#include "nsString.h"
#include "nsTArray.h"

using namespace mozilla::net;
using Part = SparseMultiRangePartWriter::Part;

// --- PushRegionSlices -----------------------------------------------------

TEST(TestSparseCacheHelpers, PushRegionSlicesValidContiguous)
{
  nsTArray<Part> out;
  // 1 KiB chunks, body covering [500..2499] (2000 bytes).
  nsAutoCString body;
  body.SetLength(2000);
  PushRegionSlices(out, body, 500, 2499, 4096, 1024);
  ASSERT_EQ(out.Length(), 3u);
  ASSERT_EQ(out[0].mRegionId, 0);
  ASSERT_EQ(out[1].mRegionId, 1);
  ASSERT_EQ(out[2].mRegionId, 2);
  ASSERT_EQ(out[0].mFirst, 500);
  ASSERT_EQ(out[0].mLast, 1023);
  ASSERT_EQ(out[1].mFirst, 1024);
  ASSERT_EQ(out[2].mLast, 2499);
}

TEST(TestSparseCacheHelpers, PushRegionSlicesNegativeFirst)
{
  nsTArray<Part> out;
  PushRegionSlices(out, "abc"_ns, -1, 5, 100, 1024);
  ASSERT_EQ(out.Length(), 0u);
}

TEST(TestSparseCacheHelpers, PushRegionSlicesLastBeforeFirst)
{
  nsTArray<Part> out;
  PushRegionSlices(out, "abc"_ns, 10, 5, 100, 1024);
  ASSERT_EQ(out.Length(), 0u);
}

TEST(TestSparseCacheHelpers, PushRegionSlicesZeroChunkSize)
{
  nsTArray<Part> out;
  PushRegionSlices(out, "abc"_ns, 0, 2, 100, 0);
  ASSERT_EQ(out.Length(), 0u);
}

TEST(TestSparseCacheHelpers, PushRegionSlicesBodyLengthMismatch)
{
  nsTArray<Part> out;
  // expected = aLast - aFirst + 1 = 100, body length = 50.
  nsAutoCString shortBody;
  shortBody.SetLength(50);
  PushRegionSlices(out, shortBody, 0, 99, 4096, 1024);
  ASSERT_EQ(out.Length(), 0u);
}

// --- ParsePartHeaders -----------------------------------------------------

TEST(TestSparseCacheHelpers, ParsePartHeadersValid)
{
  nsAutoCString body;
  body.AssignLiteral(
      "Content-Type: application/octet-stream\r\n"
      "Content-Range: bytes 100-199/4096\r\n"
      "\r\n"
      "body bytes here");
  size_t pos = 0;
  int64_t first = -1, last = -1, total = -1;
  ASSERT_TRUE(ParsePartHeaders(body, &pos, &first, &last, &total));
  ASSERT_EQ(first, 100);
  ASSERT_EQ(last, 199);
  ASSERT_EQ(total, 4096);
}

TEST(TestSparseCacheHelpers, ParsePartHeadersNoCRLF)
{
  // No \r\n anywhere → Find returns kNotFound → return false.
  nsAutoCString body;
  body.AssignLiteral("Content-Range: bytes 0-9/100no newline at end");
  size_t pos = 0;
  int64_t first = -1, last = -1, total = -1;
  ASSERT_FALSE(ParsePartHeaders(body, &pos, &first, &last, &total));
}

TEST(TestSparseCacheHelpers, ParsePartHeadersHeadersNeverEnd)
{
  // Headers present but no blank line before end of body → loop exits without
  // returning true. The trailing \r\n is consumed but there's no following
  // CRLF to mark end-of-headers.
  nsAutoCString body;
  body.AssignLiteral("Content-Range: bytes 0-9/100\r\nX-Foo: bar\r\n");
  size_t pos = 0;
  int64_t first = -1, last = -1, total = -1;
  ASSERT_FALSE(ParsePartHeaders(body, &pos, &first, &last, &total));
}

TEST(TestSparseCacheHelpers, ParsePartHeadersNoContentRange)
{
  // Blank line ends headers but no Content-Range was seen → returns
  // foundRange = false.
  nsAutoCString body;
  body.AssignLiteral("Content-Type: text/plain\r\n\r\nbody");
  size_t pos = 0;
  int64_t first = -1, last = -1, total = -1;
  ASSERT_FALSE(ParsePartHeaders(body, &pos, &first, &last, &total));
}

// --- ParseSingleSuffixOrOpenEndedByteRange --------------------------------

TEST(TestSparseCacheHelpers, ParseByteRangeSuffix)
{
  SparseMetaResolveKind kind = SparseMetaResolveKind::OpenEnded;
  int64_t n = 0;
  ASSERT_TRUE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=-500"_ns, &kind, &n));
  ASSERT_EQ(kind, SparseMetaResolveKind::Suffix);
  ASSERT_EQ(n, 500);
}

TEST(TestSparseCacheHelpers, ParseByteRangeOpenEnded)
{
  SparseMetaResolveKind kind = SparseMetaResolveKind::Suffix;
  int64_t n = -1;
  ASSERT_TRUE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=500-"_ns, &kind, &n));
  ASSERT_EQ(kind, SparseMetaResolveKind::OpenEnded);
  ASSERT_EQ(n, 500);
}

TEST(TestSparseCacheHelpers, ParseByteRangeTooShort)
{
  SparseMetaResolveKind kind;
  int64_t n;
  // < 7 characters.
  ASSERT_FALSE(ParseSingleSuffixOrOpenEndedByteRange("bytes"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeWrongPrefix)
{
  SparseMetaResolveKind kind;
  int64_t n;
  ASSERT_FALSE(
      ParseSingleSuffixOrOpenEndedByteRange("items=-500"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeMultiRange)
{
  SparseMetaResolveKind kind;
  int64_t n;
  // Comma → not a single suffix/open-ended.
  ASSERT_FALSE(ParseSingleSuffixOrOpenEndedByteRange("bytes=0-99,200-299"_ns,
                                                     &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeNoDash)
{
  SparseMetaResolveKind kind;
  int64_t n;
  ASSERT_FALSE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=12345"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeBareDash)
{
  SparseMetaResolveKind kind;
  int64_t n;
  // "bytes=-" → suffix branch with spec.Length() < 2.
  ASSERT_FALSE(ParseSingleSuffixOrOpenEndedByteRange("bytes=-"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeSuffixNonDigit)
{
  SparseMetaResolveKind kind;
  int64_t n;
  ASSERT_FALSE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=-abc"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeSuffixZero)
{
  SparseMetaResolveKind kind;
  int64_t n;
  // n <= 0 rejected (a zero-byte suffix is meaningless).
  ASSERT_FALSE(ParseSingleSuffixOrOpenEndedByteRange("bytes=-0"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeBoundedRangeRejected)
{
  SparseMetaResolveKind kind;
  int64_t n;
  // Bounded ranges (dash not at end) aren't suffix/open-ended.
  ASSERT_FALSE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=500-999"_ns, &kind, &n));
}

TEST(TestSparseCacheHelpers, ParseByteRangeOpenEndedNonDigit)
{
  SparseMetaResolveKind kind;
  int64_t n;
  ASSERT_FALSE(
      ParseSingleSuffixOrOpenEndedByteRange("bytes=abc-"_ns, &kind, &n));
}

// --- IsSingleSuffixOrOpenEndedByteRange ------------------------------------

TEST(TestSparseCacheHelpers, IsSingleSuffixOrOpenEnded)
{
  ASSERT_TRUE(IsSingleSuffixOrOpenEndedByteRange("bytes=-500"_ns));
  ASSERT_TRUE(IsSingleSuffixOrOpenEndedByteRange("bytes=500-"_ns));
  ASSERT_FALSE(IsSingleSuffixOrOpenEndedByteRange("bytes=500-999"_ns));
  ASSERT_FALSE(IsSingleSuffixOrOpenEndedByteRange("bytes=0-99,100-199"_ns));
  ASSERT_FALSE(IsSingleSuffixOrOpenEndedByteRange(""_ns));
}

// --- GenerateBoundary ------------------------------------------------------

TEST(TestSparseCacheHelpers, GenerateBoundaryNonEmptyUnique)
{
  nsAutoCString a, b;
  GenerateBoundary(a);
  GenerateBoundary(b);
  ASSERT_FALSE(a.IsEmpty());
  ASSERT_FALSE(b.IsEmpty());
  // Counter-based; two consecutive calls should produce distinct strings.
  ASSERT_NE(a, b);
}
