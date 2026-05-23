/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "nsHttp.h"
#include "nsString.h"

using namespace mozilla::net;

TEST(TestHttpByteRange, RequestBoundedRange)
{
  int64_t start = -1, end = -1;
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange("bytes=500-999"_ns, -1, &start, &end));
  ASSERT_EQ(start, 500);
  ASSERT_EQ(end, 1000);  // half-open

  // Total known: last is clamped to the entity size.
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange("bytes=500-99999"_ns, 8000, &start, &end));
  ASSERT_EQ(start, 500);
  ASSERT_EQ(end, 8000);
}

TEST(TestHttpByteRange, RequestOpenEnded)
{
  int64_t start = -1, end = -1;
  // "bytes=500-" needs the total to resolve the end.
  ASSERT_FALSE(
      nsHttp::ParseRequestByteRange("bytes=500-"_ns, -1, &start, &end));
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange("bytes=500-"_ns, 8000, &start, &end));
  ASSERT_EQ(start, 500);
  ASSERT_EQ(end, 8000);
}

TEST(TestHttpByteRange, RequestSuffix)
{
  int64_t start = -1, end = -1;
  // "bytes=-500" = last 500 bytes; needs the total.
  ASSERT_FALSE(
      nsHttp::ParseRequestByteRange("bytes=-500"_ns, -1, &start, &end));
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange("bytes=-500"_ns, 8000, &start, &end));
  ASSERT_EQ(start, 7500);
  ASSERT_EQ(end, 8000);
  // Suffix larger than the entity clamps the start to 0.
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange("bytes=-99999"_ns, 8000, &start, &end));
  ASSERT_EQ(start, 0);
  ASSERT_EQ(end, 8000);
}

TEST(TestHttpByteRange, RequestRejected)
{
  int64_t start = -1, end = -1;
  ASSERT_FALSE(nsHttp::ParseRequestByteRange("bytes=0-99,200-299"_ns, -1,
                                             &start, &end));  // multi
  ASSERT_FALSE(nsHttp::ParseRequestByteRange("items=0-5"_ns, -1, &start,
                                             &end));  // wrong unit
  ASSERT_FALSE(
      nsHttp::ParseRequestByteRange("bytes=abc-def"_ns, -1, &start, &end));
  ASSERT_FALSE(nsHttp::ParseRequestByteRange("bytes=999-500"_ns, -1, &start,
                                             &end));  // end<=start
  ASSERT_FALSE(nsHttp::ParseRequestByteRange(""_ns, -1, &start, &end));

  // Whitespace tolerated; "bytes=0-" is the cacheable whole-resource form.
  ASSERT_TRUE(
      nsHttp::ParseRequestByteRange(" bytes=0-99 "_ns, -1, &start, &end));
  ASSERT_EQ(start, 0);
  ASSERT_EQ(end, 100);
}

TEST(TestHttpByteRange, MultiRange)
{
  nsTArray<std::pair<int64_t, int64_t>> ranges;
  ASSERT_TRUE(nsHttp::ParseRequestByteRanges("bytes=0-99,200-299,500-"_ns, 800,
                                             ranges));
  ASSERT_EQ(ranges.Length(), 3u);
  ASSERT_EQ(ranges[0].first, 0);
  ASSERT_EQ(ranges[0].second, 100);
  ASSERT_EQ(ranges[1].first, 200);
  ASSERT_EQ(ranges[1].second, 300);
  ASSERT_EQ(ranges[2].first, 500);
  ASSERT_EQ(ranges[2].second, 800);  // open-ended resolved against total

  // A single bounded range is also accepted (one element).
  ASSERT_TRUE(nsHttp::ParseRequestByteRanges("bytes=10-19"_ns, -1, ranges));
  ASSERT_EQ(ranges.Length(), 1u);
  ASSERT_EQ(ranges[0].first, 10);
  ASSERT_EQ(ranges[0].second, 20);

  // A malformed part fails the whole parse.
  ASSERT_FALSE(nsHttp::ParseRequestByteRanges("bytes=0-99,bad"_ns, -1, ranges));
  ASSERT_TRUE(ranges.IsEmpty());
  // Open-ended part without a known total fails.
  ASSERT_FALSE(
      nsHttp::ParseRequestByteRanges("bytes=0-99,500-"_ns, -1, ranges));
}

TEST(TestHttpByteRange, ContentRange)
{
  int64_t first = -1, last = -1, total = -1;
  ASSERT_TRUE(nsHttp::ParseContentRangeHeader("bytes 500-999/8000"_ns, &first,
                                              &last, &total));
  ASSERT_EQ(first, 500);
  ASSERT_EQ(last, 999);
  ASSERT_EQ(total, 8000);

  // Unknown total "*".
  ASSERT_TRUE(nsHttp::ParseContentRangeHeader("bytes 0-99/*"_ns, &first, &last,
                                              &total));
  ASSERT_EQ(first, 0);
  ASSERT_EQ(last, 99);
  ASSERT_EQ(total, -1);
}

TEST(TestHttpByteRange, ContentRangeRejected)
{
  int64_t first = -1, last = -1, total = -1;
  ASSERT_FALSE(nsHttp::ParseContentRangeHeader("bytes */8000"_ns, &first, &last,
                                               &total));  // unsatisfiable
  ASSERT_FALSE(nsHttp::ParseContentRangeHeader("bytes 999-500/8000"_ns, &first,
                                               &last, &total));  // last<first
  ASSERT_FALSE(nsHttp::ParseContentRangeHeader("bytes 0-8000/8000"_ns, &first,
                                               &last, &total));  // last>=total
  ASSERT_FALSE(
      nsHttp::ParseContentRangeHeader("garbage"_ns, &first, &last, &total));
}
