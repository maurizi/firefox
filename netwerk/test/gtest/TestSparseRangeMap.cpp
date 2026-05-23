/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gtest/gtest.h"

#include "CacheFileUtils.h"
#include "nsString.h"

using namespace mozilla;
using namespace mozilla::net::CacheFileUtils;

TEST(TestSparseRangeMap, Empty)
{
  SparseRangeMap map;
  ASSERT_EQ(map.Length(), 0u);
  ASSERT_EQ(map.ValidBytes(), 0);
  ASSERT_TRUE(map.Covers(0, 0));
  ASSERT_FALSE(map.Covers(0, 1));
  ASSERT_EQ(map.FirstHoleAfter(0), 0);
  ASSERT_EQ(map.FirstHoleAfter(100), 100);

  int64_t start = -1, length = -1;
  ASSERT_FALSE(map.FirstAvailableRange(0, &start, &length));
}

TEST(TestSparseRangeMap, SingleRange)
{
  SparseRangeMap map;
  map.AddRange(100, 50);  // [100, 150)
  ASSERT_EQ(map.Length(), 1u);
  ASSERT_EQ(map.ValidBytes(), 50);

  ASSERT_TRUE(map.Covers(100, 50));
  ASSERT_TRUE(map.Covers(120, 10));
  ASSERT_TRUE(map.Covers(149, 1));
  ASSERT_FALSE(map.Covers(100, 51));
  ASSERT_FALSE(map.Covers(99, 1));
  ASSERT_FALSE(map.Covers(150, 1));

  ASSERT_EQ(map.FirstHoleAfter(100), 150);
  ASSERT_EQ(map.FirstHoleAfter(149), 150);
  ASSERT_EQ(map.FirstHoleAfter(0), 0);
  ASSERT_EQ(map.FirstHoleAfter(150), 150);
}

TEST(TestSparseRangeMap, FirstAvailableRange)
{
  SparseRangeMap map;
  map.AddRange(100, 50);   // [100, 150)
  map.AddRange(300, 100);  // [300, 400)

  int64_t start = -1, length = -1;
  // Inside the first range: run starts at the offset, length to its end.
  ASSERT_TRUE(map.FirstAvailableRange(120, &start, &length));
  ASSERT_EQ(start, 120);
  ASSERT_EQ(length, 30);
  // In the hole before the first range: returns the next run's full extent.
  ASSERT_TRUE(map.FirstAvailableRange(0, &start, &length));
  ASSERT_EQ(start, 100);
  ASSERT_EQ(length, 50);
  // In the gap between ranges: returns the second run.
  ASSERT_TRUE(map.FirstAvailableRange(200, &start, &length));
  ASSERT_EQ(start, 300);
  ASSERT_EQ(length, 100);
  // At/after the end of all data: nothing available.
  ASSERT_FALSE(map.FirstAvailableRange(400, &start, &length));
}

TEST(TestSparseRangeMap, MergeOverlapping)
{
  SparseRangeMap map;
  map.AddRange(0, 100);
  map.AddRange(50, 100);  // overlaps -> [0, 150)
  ASSERT_EQ(map.Length(), 1u);
  ASSERT_EQ(map.ValidBytes(), 150);
  ASSERT_TRUE(map.Covers(0, 150));
}

TEST(TestSparseRangeMap, MergeAdjacent)
{
  SparseRangeMap map;
  map.AddRange(0, 100);
  map.AddRange(100, 100);  // touches -> [0, 200)
  ASSERT_EQ(map.Length(), 1u);
  ASSERT_TRUE(map.Covers(50, 100));
  ASSERT_EQ(map.FirstHoleAfter(0), 200);
}

TEST(TestSparseRangeMap, MergeAbsorbsMultiple)
{
  SparseRangeMap map;
  map.AddRange(0, 10);
  map.AddRange(20, 10);
  map.AddRange(40, 10);
  ASSERT_EQ(map.Length(), 3u);
  map.AddRange(0, 50);  // swallows all three plus the gaps
  ASSERT_EQ(map.Length(), 1u);
  ASSERT_EQ(map.ValidBytes(), 50);
  ASSERT_TRUE(map.Covers(0, 50));
}

TEST(TestSparseRangeMap, DisjointAndZeroLength)
{
  SparseRangeMap map;
  map.AddRange(1000, 100);
  map.AddRange(0, 100);
  map.AddRange(500, 100);
  ASSERT_EQ(map.Length(), 3u);
  ASSERT_EQ(map.ValidBytes(), 300);
  ASSERT_FALSE(map.Covers(50, 600));  // spans holes

  map.AddRange(10, 0);
  map.AddRange(10, -5);
  ASSERT_EQ(map.Length(), 3u);  // ignored
}

TEST(TestSparseRangeMap, IsContiguousFromZero)
{
  SparseRangeMap empty;
  ASSERT_TRUE(empty.IsContiguousFromZero(0));
  ASSERT_FALSE(empty.IsContiguousFromZero(100));

  SparseRangeMap fromZero;
  fromZero.AddRange(0, 1000);
  ASSERT_TRUE(fromZero.IsContiguousFromZero(1000));
  ASSERT_FALSE(fromZero.IsContiguousFromZero(999));

  SparseRangeMap sparse;
  sparse.AddRange(100, 1000);
  ASSERT_FALSE(sparse.IsContiguousFromZero(1100));
}

TEST(TestSparseRangeMap, Truncate)
{
  SparseRangeMap map;
  map.AddRange(0, 100);
  map.AddRange(200, 100);  // [200, 300)
  map.Truncate(250);
  ASSERT_EQ(map.ValidBytes(), 150);  // [0,100) + [200,250)
  ASSERT_TRUE(map.Covers(200, 50));
  ASSERT_FALSE(map.Covers(250, 1));
}

TEST(TestSparseRangeMap, SerializeRoundTrip)
{
  SparseRangeMap map;
  map.AddRange(0, 100);
  map.AddRange(500, 100);
  map.AddRange(1LL << 40, 4096);  // deep offset, exercises 64-bit

  nsAutoCString serialized;
  map.Serialize(serialized);
  ASSERT_STREQ(serialized.get(), "0,100;500,100;1099511627776,4096;");

  SparseRangeMap parsed;
  parsed.Parse(serialized);
  ASSERT_EQ(parsed.Length(), map.Length());
  ASSERT_EQ(parsed.ValidBytes(), map.ValidBytes());
  ASSERT_TRUE(parsed.Covers(1LL << 40, 4096));

  nsAutoCString reserialized;
  parsed.Serialize(reserialized);
  ASSERT_EQ(serialized, reserialized);
}

TEST(TestSparseRangeMap, ParseMalformedStops)
{
  SparseRangeMap map;
  map.Parse("0,100;garbage"_ns);
  ASSERT_EQ(map.Length(), 1u);
  ASSERT_TRUE(map.Covers(0, 100));

  SparseRangeMap empty;
  empty.Parse(""_ns);
  ASSERT_EQ(empty.Length(), 0u);
}
