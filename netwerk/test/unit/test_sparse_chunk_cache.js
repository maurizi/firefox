// Tests sparse byte-range caching (bug 1615698): a subrange request within a
// single region is cached as its own entry and served from cache on a repeat
// request, without re-hitting the network and without over-fetching.

"use strict";

const { HttpServer } = ChromeUtils.importESModule(
  "resource://testing-common/httpd.sys.mjs"
);

let httpserver = null;
let handlerHits = 0;
let lastRangeSeen = null;
let lastIfRangeSeen = null;

// Wait for async write-through to complete: SparseWriteQueue dispatches
// runnables on the main thread, then AsyncOpenURI, then the cache write,
// then metadata flush. The interleaved executeSoon + cache flush drains
// the staged pipeline; without the spin between flushes, queued
// SparseWriteQueue runnables haven't fired yet when the flush callback
// returns.
function _flushCacheOnce() {
  return new Promise(resolve => {
    Services.cache2
      .QueryInterface(Ci.nsICacheTesting)
      .flush({ observe: resolve });
  });
}
function _yieldToEventLoop() {
  return new Promise(r => executeSoon(r));
}
async function waitForWriteThrough() {
  // More yield/flush rounds: each yield gives the main-thread event loop a
  // chance to run queued SparseWriteQueue::Drain callbacks (which dispatch
  // each next per-region writer's AsyncOpenURI). Each flush then drains the
  // cache I/O queue. Multiple rounds let a long chain of writes complete.
  for (let i = 0; i < 50; i++) {
    await _yieldToEventLoop();
    await _flushCacheOnce();
  }
}

const TOTAL = 4096;
let fullBody = "";
for (let i = 0; i < TOTAL; i++) {
  fullBody += String.fromCharCode(65 + (i % 26));
}

// Big-resource handler: 2 MiB resource for tests that exercise writes at child
// offsets past the cache's internal 256 KiB chunk boundary.
const BIG_TOTAL = 2 * 1024 * 1024;
let bigBody = "";
for (let i = 0; i < BIG_TOTAL; i++) {
  bigBody += String.fromCharCode(65 + (i % 26));
}

function rangeHandlerWith(opts) {
  return function (metadata, response) {
    handlerHits++;
    if (opts.etag !== null) {
      response.setHeader("ETag", opts.etag, false);
    }
    if (opts.lastModified !== null) {
      response.setHeader("Last-Modified", opts.lastModified, false);
    }
    if (opts.contentEncoding !== null) {
      response.setHeader("Content-Encoding", opts.contentEncoding, false);
    }
    response.setHeader("Cache-Control", "max-age=10000", false);
    response.setHeader("Accept-Ranges", "bytes", false);
    response.setHeader("Content-Type", "application/octet-stream", false);
    lastIfRangeSeen = metadata.hasHeader("If-Range")
      ? metadata.getHeader("If-Range")
      : null;
    rangeCore(metadata, response);
  };
}
function rangeCore(metadata, response) {
  if (metadata.hasHeader("Range")) {
    lastRangeSeen = metadata.getHeader("Range");
    let rangeBody = lastRangeSeen.replace(/^bytes=/, "");
    if (rangeBody.includes(",")) {
      // Multi-range -> respond with multipart/byteranges.
      let boundary = "testboundary";
      let parts = rangeBody.split(",").map(r => {
        let [s, e] = r.trim().split("-").map(Number);
        return { s, e };
      });
      response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
      response.setHeader(
        "Content-Type",
        "multipart/byteranges; boundary=" + boundary,
        false
      );
      let body = "";
      for (let p of parts) {
        body += "\r\n--" + boundary + "\r\n";
        body += "Content-Type: application/octet-stream\r\n";
        body += `Content-Range: bytes ${p.s}-${p.e}/${TOTAL}\r\n\r\n`;
        body += fullBody.substring(p.s, p.e + 1);
      }
      body += "\r\n--" + boundary + "--\r\n";
      response.bodyOutputStream.write(body, body.length);
      return;
    }
    let start, end;
    let m = /^(\d+)-(\d+)$/.exec(rangeBody);
    if (m) {
      start = parseInt(m[1]);
      end = parseInt(m[2]); // inclusive
    } else if ((m = /^-(\d+)$/.exec(rangeBody))) {
      // Suffix "bytes=-N": last N bytes.
      let n = parseInt(m[1]);
      start = Math.max(0, TOTAL - n);
      end = TOTAL - 1;
    } else if ((m = /^(\d+)-$/.exec(rangeBody))) {
      // Open-ended "bytes=N-": from N to the end.
      start = parseInt(m[1]);
      end = TOTAL - 1;
    } else {
      Assert.ok(false, "unsupported range form: " + lastRangeSeen);
      return;
    }
    let slice = fullBody.substring(start, end + 1);
    response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
    response.setHeader(
      "Content-Range",
      `bytes ${start}-${end}/${TOTAL}`,
      false
    );
    response.bodyOutputStream.write(slice, slice.length);
  } else {
    response.setStatusLine(metadata.httpVersion, 200, "OK");
    response.bodyOutputStream.write(fullBody, fullBody.length);
  }
}

const DEFAULT_HEADERS = {
  etag: '"abc"',
  lastModified: null,
  contentEncoding: null,
};
const rangeHandler = rangeHandlerWith(DEFAULT_HEADERS);
const noValidatorHandler = rangeHandlerWith({
  etag: null,
  lastModified: null,
  contentEncoding: null,
});
const weakEtagHandler = rangeHandlerWith({
  etag: 'W/"abc"',
  lastModified: null,
  contentEncoding: null,
});
const contentEncodedHandler = rangeHandlerWith({
  etag: '"abc"',
  lastModified: null,
  contentEncoding: "gzip",
});

// Used by the oversize-capture coverage tests. The body is 17 MiB, one
// byte past the 16 MiB SparseMulti/SingleRangeCaptureListener cap, so
// any request that consumes the whole body trips kMaxBuffer and the
// write-through is dropped.
const HUGE_TOTAL = 17 * 1024 * 1024;
let _hugeBody = null;
function getHugeBody() {
  if (!_hugeBody) {
    _hugeBody = "A".repeat(HUGE_TOTAL);
  }
  return _hugeBody;
}

function hugeRangeHandler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"huge"', false);
  response.setHeader("Cache-Control", "max-age=10000", false);
  response.setHeader("Accept-Ranges", "bytes", false);
  response.setHeader("Content-Type", "application/octet-stream", false);
  let body = getHugeBody();
  if (!metadata.hasHeader("Range")) {
    response.setStatusLine(metadata.httpVersion, 200, "OK");
    response.bodyOutputStream.write(body, body.length);
    return;
  }
  let rangeBody = metadata.getHeader("Range").replace(/^bytes=/, "");
  if (rangeBody.includes(",")) {
    let boundary = "hugeboundary";
    let parts = rangeBody.split(",").map(r => {
      let [s, e] = r.trim().split("-").map(Number);
      return { s, e };
    });
    response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
    response.setHeader(
      "Content-Type",
      "multipart/byteranges; boundary=" + boundary,
      false
    );
    for (let p of parts) {
      let head = "\r\n--" + boundary + "\r\n";
      head += "Content-Type: application/octet-stream\r\n";
      head += `Content-Range: bytes ${p.s}-${p.e}/${HUGE_TOTAL}\r\n\r\n`;
      response.bodyOutputStream.write(head, head.length);
      let slice = body.substring(p.s, p.e + 1);
      response.bodyOutputStream.write(slice, slice.length);
    }
    let trailer = "\r\n--" + boundary + "--\r\n";
    response.bodyOutputStream.write(trailer, trailer.length);
    return;
  }
  let m = /^(\d+)-(\d+)$/.exec(rangeBody);
  if (!m) {
    Assert.ok(false, "unsupported range: " + rangeBody);
    return;
  }
  let start = parseInt(m[1]);
  let end = parseInt(m[2]);
  let slice = body.substring(start, end + 1);
  response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
  response.setHeader(
    "Content-Range",
    `bytes ${start}-${end}/${HUGE_TOTAL}`,
    false
  );
  response.bodyOutputStream.write(slice, slice.length);
}

function bigRangeHandler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"big"', false);
  response.setHeader("Cache-Control", "max-age=10000", false);
  response.setHeader("Accept-Ranges", "bytes", false);
  response.setHeader("Content-Type", "application/octet-stream", false);
  if (!metadata.hasHeader("Range")) {
    response.setStatusLine(metadata.httpVersion, 200, "OK");
    response.bodyOutputStream.write(bigBody, bigBody.length);
    return;
  }
  let rangeBody = metadata.getHeader("Range").replace(/^bytes=/, "");
  // Multi-range support: synthesize multipart/byteranges (used by the
  // cross-chunk-write-order regression test).
  if (rangeBody.includes(",")) {
    let boundary = "bigboundary";
    let parts = rangeBody.split(",").map(r => {
      let [s, e] = r.trim().split("-").map(Number);
      return { s, e };
    });
    response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
    response.setHeader(
      "Content-Type",
      "multipart/byteranges; boundary=" + boundary,
      false
    );
    let body = "";
    for (let p of parts) {
      body += "\r\n--" + boundary + "\r\n";
      body += "Content-Type: application/octet-stream\r\n";
      body += `Content-Range: bytes ${p.s}-${p.e}/${BIG_TOTAL}\r\n\r\n`;
      body += bigBody.substring(p.s, p.e + 1);
    }
    body += "\r\n--" + boundary + "--\r\n";
    response.bodyOutputStream.write(body, body.length);
    return;
  }
  let start, end;
  let m = /^(\d+)-(\d+)$/.exec(rangeBody);
  if (m) {
    start = parseInt(m[1]);
    end = parseInt(m[2]);
  } else if ((m = /^-(\d+)$/.exec(rangeBody))) {
    start = Math.max(0, BIG_TOTAL - parseInt(m[1]));
    end = BIG_TOTAL - 1;
  } else if ((m = /^(\d+)-$/.exec(rangeBody))) {
    start = parseInt(m[1]);
    end = BIG_TOTAL - 1;
  } else {
    Assert.ok(false, "unsupported range: " + rangeBody);
    return;
  }
  let slice = bigBody.substring(start, end + 1);
  response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
  response.setHeader(
    "Content-Range",
    `bytes ${start}-${end}/${BIG_TOTAL}`,
    false
  );
  response.bodyOutputStream.write(slice, slice.length);
}

// "Lying" handler: a server that always returns the same canned 206 with
// Content-Range `bytes 0-99/4096`, regardless of the request's Range. Used to
// exercise strict Content-Range validation.
function lyingWrongRangeHandler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"abc"', false);
  response.setHeader("Cache-Control", "max-age=10000", false);
  response.setHeader("Accept-Ranges", "bytes", false);
  response.setHeader("Content-Type", "application/octet-stream", false);
  response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
  response.setHeader("Content-Range", `bytes 0-99/${TOTAL}`, false);
  let slice = fullBody.substring(0, 100);
  response.bodyOutputStream.write(slice, slice.length);
}

// Returns the actual entity tail when asked for bytes past EOF: a legitimate
// clamp-to-EOF the cache should accept.
function clampEofHandler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"abc"', false);
  response.setHeader("Cache-Control", "max-age=10000", false);
  response.setHeader("Accept-Ranges", "bytes", false);
  response.setHeader("Content-Type", "application/octet-stream", false);
  // Always returns bytes 3500-4095/4096, no matter what the client asked for.
  // For the test we only call it with bytes=3500-99999 → server clamps to EOF.
  response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
  response.setHeader("Content-Range", `bytes 3500-4095/${TOTAL}`, false);
  let slice = fullBody.substring(3500, 4096);
  response.bodyOutputStream.write(slice, slice.length);
}

// Returns 416 for every request.
function always416Handler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"abc"', false);
  response.setHeader("Content-Range", `bytes */${TOTAL}`, false);
  response.setStatusLine(metadata.httpVersion, 416, "Range Not Satisfiable");
}

// Returns a multipart/byteranges response with one solicited part and one
// unsolicited part (a small range the client never asked for).
function lyingExtraMultirangeHandler(metadata, response) {
  handlerHits++;
  response.setHeader("ETag", '"abc"', false);
  response.setHeader("Cache-Control", "max-age=10000", false);
  response.setHeader("Accept-Ranges", "bytes", false);
  let boundary = "extrabd";
  response.setHeader(
    "Content-Type",
    "multipart/byteranges; boundary=" + boundary,
    false
  );
  response.setStatusLine(metadata.httpVersion, 206, "Partial Content");
  // Solicited (the test asks for bytes=100-199,500-599) plus one unsolicited.
  let body = "";
  for (let p of [
    { s: 100, e: 199 },
    { s: 500, e: 599 },
    { s: 2000, e: 2099 },
  ]) {
    body += "\r\n--" + boundary + "\r\n";
    body += "Content-Type: application/octet-stream\r\n";
    body += `Content-Range: bytes ${p.s}-${p.e}/${TOTAL}\r\n\r\n`;
    body += fullBody.substring(p.s, p.e + 1);
  }
  body += "\r\n--" + boundary + "--\r\n";
  response.bodyOutputStream.write(body, body.length);
}

let etagCounter = 0;
function rotatingEtagHandler(metadata, response) {
  etagCounter++;
  const handler = rangeHandlerWith({
    etag: `"v${etagCounter}"`,
    lastModified: null,
    contentEncoding: null,
  });
  return handler(metadata, response);
}

function makeChannel(url) {
  return NetUtil.newChannel({
    uri: url,
    loadUsingSystemPrincipal: true,
  }).QueryInterface(Ci.nsIHttpChannel);
}

function requestRange(url, rangeValue, opts = {}) {
  return new Promise(resolve => {
    let chan = makeChannel(url);
    chan.setRequestHeader("Range", rangeValue, false);
    if (opts.bypassCache) {
      chan.loadFlags |= Ci.nsIRequest.LOAD_BYPASS_CACHE;
    }
    let data = "";
    let listener = {
      QueryInterface: ChromeUtils.generateQI([
        "nsIStreamListener",
        "nsIRequestObserver",
      ]),
      onStartRequest() {},
      onDataAvailable(req, stream, offset, count) {
        data += read_stream(stream, count);
      },
      onStopRequest(req, status) {
        req.QueryInterface(Ci.nsIHttpChannel);
        let code = -1;
        try {
          code = req.responseStatus;
        } catch (e) {}
        let contentRange = null;
        let contentType = null;
        try {
          contentRange = req.getResponseHeader("Content-Range");
        } catch (e) {}
        try {
          contentType = req.getResponseHeader("Content-Type");
        } catch (e) {}
        resolve({ status, data, code, contentRange, contentType });
      },
    };
    chan.asyncOpen(listener);
  });
}

add_task(async function setup() {
  Services.prefs.setBoolPref("network.http.sparse_entries.enabled", true);
  httpserver = new HttpServer();
  httpserver.registerPathHandler("/cog", rangeHandler);
  httpserver.registerPathHandler("/cog2", rangeHandler);
  httpserver.registerPathHandler("/cog3", rangeHandler);
  httpserver.registerPathHandler("/cog4", rangeHandler);
  httpserver.registerPathHandler("/cog5", rangeHandler);
  httpserver.registerPathHandler("/cog6", rangeHandler);
  httpserver.registerPathHandler("/cog7", rangeHandler);
  httpserver.registerPathHandler("/cog8", rangeHandler);
  httpserver.registerPathHandler("/cog9", rangeHandler);
  httpserver.registerPathHandler("/cog10", rangeHandler);
  httpserver.registerPathHandler("/cog11", rangeHandler);
  httpserver.registerPathHandler("/cog12", rangeHandler);
  httpserver.registerPathHandler("/cog13", rangeHandler);
  httpserver.registerPathHandler("/cog14", rangeHandler);
  httpserver.registerPathHandler("/cog15", rangeHandler);
  httpserver.registerPathHandler("/cog16", rangeHandler);
  httpserver.registerPathHandler("/cog17", rangeHandler);
  httpserver.registerPathHandler("/cog18", rangeHandler);
  httpserver.registerPathHandler("/cog_concurrent_same_region", rangeHandler);
  httpserver.registerPathHandler("/cog_concurrent_multi_region", rangeHandler);
  httpserver.registerPathHandler("/cog_concurrent_multirange", rangeHandler);
  httpserver.registerPathHandler("/cog_cross_chunk_order", bigRangeHandler);
  httpserver.registerPathHandler("/lying_wrong_range", lyingWrongRangeHandler);
  httpserver.registerPathHandler("/clamp_eof", clampEofHandler);
  httpserver.registerPathHandler("/always_416", always416Handler);
  httpserver.registerPathHandler(
    "/lying_extra_multirange",
    lyingExtraMultirangeHandler
  );
  httpserver.registerPathHandler("/no_validator", noValidatorHandler);
  httpserver.registerPathHandler("/weak_etag", weakEtagHandler);
  httpserver.registerPathHandler("/content_encoded", contentEncodedHandler);
  httpserver.registerPathHandler("/rotating_etag", rotatingEtagHandler);
  httpserver.registerPathHandler("/huge_single", hugeRangeHandler);
  httpserver.registerPathHandler("/huge_multi", hugeRangeHandler);
  httpserver.registerPathHandler("/cog_open_ended_beyond", rangeHandler);
  httpserver.registerPathHandler("/cog_expired_region", rangeHandler);
  httpserver.registerPathHandler("/cog_brand_new", rangeHandler);
  httpserver.registerPathHandler("/big", bigRangeHandler);
  httpserver.start(-1);
  registerCleanupFunction(() => {
    Services.prefs.clearUserPref("network.http.sparse_entries.chunk_size");
  });
  registerCleanupFunction(async () => {
    Services.prefs.clearUserPref("network.http.sparse_entries.enabled");
    await new Promise(r => httpserver.stop(r));
  });
});

add_task(async function test_store_then_serve() {
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog`;

  // First request for [500, 1000): miss -> network.
  handlerHits = 0;
  lastRangeSeen = null;
  let r1 = await requestRange(url, "bytes=500-999");
  Assert.equal(r1.status, Cr.NS_OK, "first request succeeded");
  Assert.equal(r1.code, 206, "first response is 206");
  Assert.equal(r1.data, fullBody.substring(500, 1000), "first bytes correct");
  Assert.equal(
    r1.contentRange,
    `bytes 500-999/${TOTAL}`,
    "first Content-Range"
  );
  Assert.equal(handlerHits, 1, "network hit exactly once");
  Assert.equal(
    lastRangeSeen,
    "bytes=500-999",
    "no over-fetch: server saw the exact requested range"
  );

  // Second identical request: must be served from cache, no network hit.
  let r2 = await requestRange(url, "bytes=500-999");
  Assert.equal(r2.status, Cr.NS_OK, "second request succeeded");
  Assert.equal(r2.code, 206, "second response is 206 (from cache)");
  Assert.equal(r2.data, fullBody.substring(500, 1000), "second bytes correct");
  Assert.equal(
    r2.contentRange,
    `bytes 500-999/${TOTAL}`,
    "second Content-Range"
  );
  Assert.equal(handlerHits, 1, "no additional network hit (served from cache)");
});

// Two distinct sub-ranges of the same region accumulate in one region entry;
// both are afterwards served from cache.
add_task(async function test_accumulate_subranges() {
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog2`;
  handlerHits = 0;

  // Fetch two disjoint sub-ranges in region 0 -> two network hits.
  let a1 = await requestRange(url, "bytes=0-99");
  Assert.equal(a1.data, fullBody.substring(0, 100), "range A bytes");
  let b1 = await requestRange(url, "bytes=2000-2099");
  Assert.equal(b1.data, fullBody.substring(2000, 2100), "range B bytes");
  Assert.equal(handlerHits, 2, "two distinct sub-ranges fetched from network");

  // Both are now cached in the same region entry: re-requesting either is a
  // cache hit (no new network fetch). This is the accumulation guarantee.
  let a2 = await requestRange(url, "bytes=0-99");
  Assert.equal(a2.code, 206, "range A re-served as 206");
  Assert.equal(
    a2.data,
    fullBody.substring(0, 100),
    "range A served from cache"
  );
  Assert.equal(handlerHits, 2, "range A served from cache, no network");

  let b2 = await requestRange(url, "bytes=2000-2099");
  Assert.equal(b2.code, 206, "range B re-served as 206");
  Assert.equal(
    b2.data,
    fullBody.substring(2000, 2100),
    "range B served from cache"
  );
  Assert.equal(
    handlerHits,
    2,
    "range B still cached after range A — both sub-ranges coexist"
  );

  // A sub-range that was never fetched is still a miss (goes to network).
  let c1 = await requestRange(url, "bytes=500-599");
  Assert.equal(c1.data, fullBody.substring(500, 600), "range C bytes");
  Assert.equal(handlerHits, 3, "uncached sub-range fetched from network");
});

// A sub-range of a resource whose *total* size exceeds the per-entry cache
// size limit must still cache: the region entry only holds the small slice, so
// the size prediction must be the slice, not the whole entity. (Regression for
// the bug where every region of a >max_entry_size file was doomed on store.)
add_task(async function test_resource_larger_than_entry_limit() {
  // Limit entries to 1 KiB. The test entity is 4 KiB (> limit) but each
  // requested slice is well under 1 KiB.
  Services.prefs.setIntPref("browser.cache.disk.max_entry_size", 1);
  Services.prefs.setIntPref("browser.cache.memory.max_entry_size", 1);

  let url = `http://localhost:${httpserver.identity.primaryPort}/cog4`;
  handlerHits = 0;

  // bytes=100-199 is a 100-byte slice of the 4096-byte (over-limit) entity.
  let r1 = await requestRange(url, "bytes=100-199");
  Assert.equal(r1.data, fullBody.substring(100, 200), "slice bytes correct");
  Assert.equal(handlerHits, 1, "first slice fetched from network");

  let r2 = await requestRange(url, "bytes=100-199");
  Assert.equal(r2.code, 206, "slice re-served as 206");
  Assert.equal(
    r2.data,
    fullBody.substring(100, 200),
    "slice served from cache"
  );
  Assert.equal(
    handlerHits,
    1,
    "slice of an over-limit resource is cached and served (not doomed)"
  );

  Services.prefs.clearUserPref("browser.cache.disk.max_entry_size");
  Services.prefs.clearUserPref("browser.cache.memory.max_entry_size");
});

// A request spanning multiple regions is served from cache when every region's
// slice is present (assembled via the multi-region coordinator); otherwise it
// goes to the network for the full range.
add_task(async function test_multiregion() {
  // Small regions so requests span region boundaries.
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog3`;
  handlerHits = 0;

  // Populate region 0's [500,1024) and region 1's [1024,1500) via single-region
  // requests.
  await requestRange(url, "bytes=500-1023");
  await requestRange(url, "bytes=1024-1499");
  Assert.equal(
    handlerHits,
    2,
    "two single-region fetches populated the regions"
  );

  // A request spanning region 0 and region 1, fully covered -> served from
  // cache by assembling the two region slices.
  let span = await requestRange(url, "bytes=500-1499");
  Assert.equal(span.code, 206, "multi-region served as 206");
  Assert.equal(
    span.data,
    fullBody.substring(500, 1500),
    "multi-region bytes assembled"
  );
  Assert.equal(
    span.contentRange,
    `bytes 500-1499/${TOTAL}`,
    "multi-region Content-Range"
  );
  Assert.equal(handlerHits, 2, "multi-region served from cache, no network");

  // A spanning request whose regions aren't all cached -> full network fetch.
  let miss = await requestRange(url, "bytes=500-2500");
  Assert.equal(miss.data, fullBody.substring(500, 2501), "miss bytes correct");
  Assert.greater(handlerHits, 2, "uncovered multi-region went to network");
});

// A multi-range request (bytes=a-b,c-d) is served from cache as a synthesized
// multipart/byteranges response when every sub-range is already cached.
add_task(async function test_multirange_serve() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog5`;
  handlerHits = 0;

  // Populate two sub-ranges in different regions via single-range requests.
  await requestRange(url, "bytes=100-199");
  await requestRange(url, "bytes=1100-1199");
  Assert.equal(
    handlerHits,
    2,
    "two single-range fetches populated the regions"
  );

  let r = await requestRange(url, "bytes=100-199,1100-1199");
  Assert.equal(r.code, 206, "multi-range served as 206 from cache");
  Assert.equal(handlerHits, 2, "no network hit for fully-covered multi-range");
  Assert.ok(
    r.contentType && r.contentType.startsWith("multipart/byteranges"),
    "Content-Type is multipart/byteranges: " + r.contentType
  );
  // Body should contain a part Content-Range for each sub-range and the bytes.
  Assert.ok(
    r.data.includes("Content-Range: bytes 100-199/4096"),
    "first part has correct Content-Range"
  );
  Assert.ok(
    r.data.includes("Content-Range: bytes 1100-1199/4096"),
    "second part has correct Content-Range"
  );
  Assert.ok(
    r.data.includes(fullBody.substring(100, 200)),
    "body contains first sub-range's bytes"
  );
  Assert.ok(
    r.data.includes(fullBody.substring(1100, 1200)),
    "body contains second sub-range's bytes"
  );
});

// A multi-range MISS fetches multipart/byteranges from the network and writes
// each part through to its region cache entry. A subsequent single-range
// request for one of those sub-ranges then serves from cache.
add_task(async function test_multirange_store_write_through() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog6`;
  handlerHits = 0;

  // Multi-range MISS -> server returns multipart/byteranges; we write each
  // part into its region entry as a side-effect.
  let miss = await requestRange(url, "bytes=200-299,1200-1299");
  Assert.equal(miss.code, 206, "multi-range miss is 206");
  Assert.ok(
    miss.contentType && miss.contentType.startsWith("multipart/byteranges"),
    "miss is multipart/byteranges: " + miss.contentType
  );
  Assert.equal(handlerHits, 1, "miss hit the network once");

  // Give the async write-through time to complete (cache IO + metadata).
  await waitForWriteThrough();

  // A single-range request for one of the written sub-ranges should serve
  // from cache (no network hit).
  let hit = await requestRange(url, "bytes=200-299");
  Assert.equal(hit.code, 206, "single-range follow-up is 206");
  Assert.equal(
    hit.data,
    fullBody.substring(200, 300),
    "follow-up bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "single-range sub-range served from cache after multi-range write-through"
  );

  // The other sub-range should also be cached.
  let hit2 = await requestRange(url, "bytes=1200-1299");
  Assert.equal(hit2.code, 206, "other sub-range is 206");
  Assert.equal(hit2.data, fullBody.substring(1200, 1300));
  Assert.equal(handlerHits, 1, "both written sub-ranges served from cache");
});

// A multi-region single-range MISS fetches the full span from the network and
// the single-range capture splits the response body across the spanned regions,
// writing each slice through to its region cache entry.
add_task(async function test_multiregion_miss_write_through() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog7`;
  handlerHits = 0;

  // Spanning request bytes=500-2500 covers region 0 [500..1023], region 1
  // [1024..2047] and region 2 [2048..2500].
  let miss = await requestRange(url, "bytes=500-2500");
  Assert.equal(miss.code, 206, "multi-region miss is 206");
  Assert.equal(miss.data, fullBody.substring(500, 2501), "miss bytes correct");
  Assert.equal(handlerHits, 1, "miss hit the network once");

  await waitForWriteThrough();

  let r0 = await requestRange(url, "bytes=500-1023");
  Assert.equal(r0.code, 206, "region 0 slice is 206");
  Assert.equal(r0.data, fullBody.substring(500, 1024), "region 0 slice bytes");
  Assert.equal(handlerHits, 1, "region 0 slice served from cache");

  let r1 = await requestRange(url, "bytes=1024-2047");
  Assert.equal(r1.code, 206, "region 1 slice is 206");
  Assert.equal(r1.data, fullBody.substring(1024, 2048), "region 1 slice bytes");
  Assert.equal(handlerHits, 1, "region 1 slice served from cache");

  let r2 = await requestRange(url, "bytes=2048-2500");
  Assert.equal(r2.code, 206, "region 2 slice is 206");
  Assert.equal(r2.data, fullBody.substring(2048, 2501), "region 2 slice bytes");
  Assert.equal(handlerHits, 1, "region 2 slice served from cache");
});

// A multi-range MISS whose first part spans regions: the multipart parser must
// split that part across both spanned regions and write each slice through.
add_task(async function test_multirange_spanning_part_write_through() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog8`;
  handlerHits = 0;

  // Part A bytes=500-1499 spans regions 0 and 1; part B bytes=2000-2099 lives
  // in region 1.
  let miss = await requestRange(url, "bytes=500-1499,2000-2099");
  Assert.equal(miss.code, 206, "multi-range miss is 206");
  Assert.ok(
    miss.contentType && miss.contentType.startsWith("multipart/byteranges"),
    "miss is multipart/byteranges"
  );
  Assert.equal(handlerHits, 1, "miss hit the network once");

  await waitForWriteThrough();

  let r0 = await requestRange(url, "bytes=500-1023");
  Assert.equal(r0.code, 206, "spanning part's r0 slice is 206");
  Assert.equal(r0.data, fullBody.substring(500, 1024));
  Assert.equal(handlerHits, 1, "spanning part's r0 slice served from cache");

  let r1 = await requestRange(url, "bytes=1024-1499");
  Assert.equal(r1.code, 206, "spanning part's r1 slice is 206");
  Assert.equal(r1.data, fullBody.substring(1024, 1500));
  Assert.equal(handlerHits, 1, "spanning part's r1 slice served from cache");

  let r2 = await requestRange(url, "bytes=2000-2099");
  Assert.equal(r2.code, 206, "non-spanning part is 206");
  Assert.equal(r2.data, fullBody.substring(2000, 2100));
  Assert.equal(handlerHits, 1, "non-spanning part served from cache");
});

// A suffix range "bytes=-N" can't be mapped to a region at request time (no
// known total), but the response Content-Range supplies it. The single-range
// capture writes the bytes through to the region(s) afterwards.
add_task(async function test_suffix_range_write_through() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog9`;
  handlerHits = 0;

  // Last 500 bytes -> [3596..4095] (region 3).
  let suffix = await requestRange(url, "bytes=-500");
  Assert.equal(suffix.code, 206, "suffix range is 206");
  Assert.equal(
    suffix.data,
    fullBody.substring(3596, 4096),
    "suffix bytes correct"
  );
  Assert.equal(handlerHits, 1, "suffix went to network");

  await waitForWriteThrough();

  let hit = await requestRange(url, "bytes=3596-4095");
  Assert.equal(hit.code, 206, "follow-up bounded range is 206");
  Assert.equal(hit.data, fullBody.substring(3596, 4096));
  Assert.equal(handlerHits, 1, "suffix-written bytes served from cache");
});

// An open-ended "bytes=N-" range needs the response Content-Range for the
// total. After write-through, a bounded sub-range of the same bytes serves
// from cache.
add_task(async function test_open_ended_range_write_through() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog10`;
  handlerHits = 0;

  // From 3500 to the end -> [3500..4095].
  let open = await requestRange(url, "bytes=3500-");
  Assert.equal(open.code, 206, "open-ended is 206");
  Assert.equal(
    open.data,
    fullBody.substring(3500, 4096),
    "open-ended bytes correct"
  );
  Assert.equal(handlerHits, 1, "open-ended went to network");

  await waitForWriteThrough();

  let hit = await requestRange(url, "bytes=3500-4095");
  Assert.equal(hit.code, 206, "follow-up bounded range is 206");
  Assert.equal(hit.data, fullBody.substring(3500, 4096));
  Assert.equal(handlerHits, 1, "open-ended-written bytes served from cache");
});

// A response with no strong validator (no ETag and no Last-Modified) must not
// be sparse-cached: a later request for the same range refetches from network.
add_task(async function test_no_validator_not_cached() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/no_validator`;
  handlerHits = 0;

  let r1 = await requestRange(url, "bytes=500-999");
  Assert.equal(r1.code, 206);
  Assert.equal(r1.data, fullBody.substring(500, 1000));
  Assert.equal(handlerHits, 1, "first fetch hit the network");

  let r2 = await requestRange(url, "bytes=500-999");
  Assert.equal(r2.code, 206);
  Assert.equal(
    handlerHits,
    2,
    "no strong validator => response not sparse-cached"
  );
});

// A weak ETag (W/"...") is not a usable validator for If-Range; the response
// must not be sparse-cached.
add_task(async function test_weak_etag_not_cached() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/weak_etag`;
  handlerHits = 0;

  await requestRange(url, "bytes=500-999");
  await requestRange(url, "bytes=500-999");
  Assert.equal(handlerHits, 2, "weak ETag => response not sparse-cached");
});

// Content-Encoding is over the encoded representation; range slices don't
// compose into a coherent decoded entity, so the response must not be
// sparse-cached.
add_task(async function test_content_encoded_not_cached() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/content_encoded`;
  handlerHits = 0;
  // The "gzip" payload isn't actually gzip; the consumer may error on
  // decompression but that's fine — we only care that no sparse write-through
  // happened.
  try {
    await requestRange(url, "bytes=500-999");
  } catch (e) {}
  try {
    await requestRange(url, "bytes=500-999");
  } catch (e) {}
  Assert.equal(
    handlerHits,
    2,
    "Content-Encoding => response not sparse-cached"
  );
});

// When refetching a sub-range from a region that already holds other cached
// bytes, the channel must send If-Range with the stored strong ETag so the
// server can detect a changed resource.
add_task(async function test_if_range_on_refetch() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog11`;
  handlerHits = 0;
  lastIfRangeSeen = "<unset>";

  // First fetch: no If-Range (nothing cached yet for this URL).
  let r1 = await requestRange(url, "bytes=0-99");
  Assert.equal(r1.code, 206);
  Assert.equal(handlerHits, 1);
  Assert.equal(lastIfRangeSeen, null, "no If-Range on the cold fetch");

  // Second fetch: a different sub-range in the same region. The region entry
  // is fresh but the requested range isn't cached, so the channel refetches —
  // the refetch must carry If-Range matching the stored strong ETag.
  let r2 = await requestRange(url, "bytes=200-299");
  Assert.equal(r2.code, 206);
  Assert.equal(handlerHits, 2);
  Assert.equal(
    lastIfRangeSeen,
    '"abc"',
    "refetch carried If-Range with stored ETag"
  );
});

// LOAD_BYPASS_CACHE (DevTools "Disable cache" / Cmd-Shift-R) must skip the
// sparse cache path entirely — sub-range requests with the flag go to the
// network even when a covering region entry exists.
add_task(async function test_bypass_local_cache_skips_sparse() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog12`;
  handlerHits = 0;

  // Cold fetch populates the region entry.
  await requestRange(url, "bytes=500-999");
  Assert.equal(handlerHits, 1, "cold fetch hit the network");

  // Repeat with LOAD_BYPASS_CACHE — must hit the network again, not the cache.
  let r = await requestRange(url, "bytes=500-999", { bypassCache: true });
  Assert.equal(r.code, 206, "bypass fetch is 206");
  Assert.equal(r.data, fullBody.substring(500, 1000), "bypass bytes correct");
  Assert.equal(handlerHits, 2, "LOAD_BYPASS_CACHE forced a network fetch");
});

// A suffix range whose absolute offset within its region is past the cache
// file's internal 256 KiB chunk boundary. The write-through writer opens the
// region entry's output stream at that child offset, which forces the cache
// layer to create / fill the preceding chunks of the otherwise-fresh entry.
// Regression for a browser crash reproduced via bytes=-262144 on a multi-MB
// resource.
add_task(async function test_suffix_high_child_offset_no_crash() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1048576);
  let url = `http://localhost:${httpserver.identity.primaryPort}/big`;
  handlerHits = 0;

  // bytes=-300000 -> [BIG_TOTAL-300000, BIG_TOTAL-1] = [1797152, 2097151].
  // region 1 (1 MiB), childOffset = 748576 (well past chunk boundary 262144).
  let suffix = await requestRange(url, "bytes=-300000");
  Assert.equal(suffix.code, 206, "suffix is 206");
  Assert.equal(
    suffix.data,
    bigBody.substring(BIG_TOTAL - 300000, BIG_TOTAL),
    "suffix bytes correct"
  );
  Assert.equal(handlerHits, 1, "suffix went to network");

  // Wait for write-through to complete.
  await waitForWriteThrough();

  // A follow-up sub-range within the written window must serve from cache —
  // and equally importantly, the write-through must not have crashed.
  let hit = await requestRange(url, "bytes=1900000-2000000");
  Assert.equal(hit.code, 206, "follow-up is 206");
  Assert.equal(
    hit.data,
    bigBody.substring(1900000, 2000001),
    "follow-up bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "follow-up sub-range served from cache (no crash, write-through landed)"
  );
});

// A region whose stored ranges leave a gap *right after* a windowed read
// must still serve that windowed read cleanly. The slice's underlying
// CacheFileInputStream needs SetReadEndBound so it returns clean EOF at the
// slice boundary instead of consulting the sparse map and tripping on the
// adjacent hole (regression for the NS_ERROR_CACHE_DATA_INCOMPLETE the
// channel was reporting on completed reads).
add_task(async function test_singleregion_serve_with_adjacent_hole() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog13`;
  handlerHits = 0;

  // Two non-adjacent sub-ranges in region 0: [500,1000) and [2000,3000).
  // The gap between them is exactly at the end of the first sub-range, so a
  // re-read of bytes=500-999 ends at mPos=1000 — right at the start of the
  // hole [1000, 2000).
  await requestRange(url, "bytes=500-999");
  await requestRange(url, "bytes=2000-2999");
  Assert.equal(handlerHits, 2, "two sub-ranges populated region 0");

  // Re-read the first sub-range. Must complete with NS_OK status (the
  // consumer reads exactly 500 bytes and gets clean EOF at the boundary).
  let r = await requestRange(url, "bytes=500-999");
  Assert.equal(r.status, Cr.NS_OK, "boundary-adjacent-hole serve succeeded");
  Assert.equal(r.code, 206, "served as 206 from cache");
  Assert.equal(r.data, fullBody.substring(500, 1000), "bytes correct");
  Assert.equal(
    handlerHits,
    2,
    "no network hit (cache serve with adjacent hole completed cleanly)"
  );
});

// Same boundary-EOF protection but exercised through the multi-region serve
// coordinator: each spanned region's slice may end at a hole, and reading
// across regions must not error mid-stream when the multiplex pulls the next
// slice (regression for the multi-region hang).
add_task(async function test_multiregion_serve_with_adjacent_holes() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog14`;
  handlerHits = 0;

  // Region 0: sparse with a hole right after the slice we'll read.
  //   [200, 500) and [600, 1024) — gap at [500, 600).
  // Region 1: fully covered [0, 500).
  // Multi-region request bytes=200-1499 spans region 0 slice (childOffset=200,
  // sliceLen=824 — covers [200, 1024) which crosses the hole [500, 600))
  // …actually that would be a partial coverage so the reader misses. Use
  // bytes=200-499 + bytes=600-1023 to populate region 0, then bytes=600-1499
  // for the spanning request — region 0 slice [600, 1024) is fully cached
  // (single run) and ends exactly at the start of the chunk-region boundary,
  // and region 1 slice [0, 476) is fully cached too.
  await requestRange(url, "bytes=200-499"); // region 0 [200, 500)
  await requestRange(url, "bytes=600-1023"); // region 0 [600, 1024)
  await requestRange(url, "bytes=1024-1499"); // region 1 [0, 476)
  Assert.equal(handlerHits, 3, "three sub-ranges populated region 0 + 1");

  // Multi-region request: region 0 slice covers [600, 1024) — bounded by
  // SetReadEndBound at the slice end, which is exactly the start of region 1.
  // Region 0 is sparse (has the [200,500) range *and* the [600,1024) range —
  // the [500,600) hole sits right before the slice). Region 1 slice [0, 476)
  // ends at the run boundary too. Both ends must EOF cleanly so the multiplex
  // can advance from slice 0 to slice 1 without surfacing an error.
  let r = await requestRange(url, "bytes=600-1499");
  Assert.equal(r.status, Cr.NS_OK, "multi-region serve with holes succeeded");
  Assert.equal(r.code, 206, "served as 206 from cache");
  Assert.equal(
    r.data,
    fullBody.substring(600, 1500),
    "assembled bytes correct"
  );
  Assert.equal(
    handlerHits,
    3,
    "multi-region served from cache; no network hit"
  );
});

// After a bounded sub-range populates the per-URL :sparsemeta entry with the
// entity total, a subsequent suffix request must resolve via meta and serve
// the suffix bytes from cache (no network round-trip to discover the total).
add_task(async function test_sparsemeta_suffix_served_from_cache() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog16`;
  handlerHits = 0;

  // First request seeds region 3 (the last 500 bytes of a 4096-byte resource
  // live there) AND the :sparsemeta entry with total=4096.
  let prime = await requestRange(url, "bytes=3596-4095");
  Assert.equal(prime.code, 206, "prime fetch is 206");
  Assert.equal(handlerHits, 1, "prime hit network once");

  await waitForWriteThrough();

  // Now ask for the same bytes via a suffix. The resolver should learn
  // total=4096 from :sparsemeta and map to bytes 3596-4095 which is already
  // cached in region 3.
  let suffix = await requestRange(url, "bytes=-500");
  Assert.equal(suffix.code, 206, "suffix served as 206");
  Assert.equal(
    suffix.data,
    fullBody.substring(3596, 4096),
    "suffix bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "suffix served from cache via :sparsemeta resolve"
  );
});

// Same shape for open-ended requests.
add_task(async function test_sparsemeta_open_ended_served_from_cache() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog17`;
  handlerHits = 0;

  // Multi-region prime that writes through region 3 AND populates meta.
  let prime = await requestRange(url, "bytes=3500-4095");
  Assert.equal(prime.code, 206, "prime fetch is 206");
  Assert.equal(handlerHits, 1);

  await waitForWriteThrough();

  // Open-ended bytes=3500-. Resolver learns total=4096 from meta and maps
  // to bytes 3500-4095 (in region 3, which we cached above).
  let open = await requestRange(url, "bytes=3500-");
  Assert.equal(open.code, 206, "open-ended served as 206");
  Assert.equal(
    open.data,
    fullBody.substring(3500, 4096),
    "open-ended bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "open-ended served from cache via :sparsemeta resolve"
  );
});

// Multi-region resolved suffix: prime a span that covers two regions, then
// the suffix resolver maps `bytes=-2048` to bytes 2048-4095, which crosses
// the region boundary and triggers OnSparseSpanReady. The synthesized 206
// must come back with the right Content-Range and the right bytes — proves
// the meta-cached response head feeds BuildSparse206Head correctly.
add_task(async function test_sparsemeta_multiregion_suffix_from_cache() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog18`;
  handlerHits = 0;

  // Bounded multi-region miss: writes through both region 2 and region 3
  // AND populates :sparsemeta with total=4096.
  let prime = await requestRange(url, "bytes=2048-4095");
  Assert.equal(prime.code, 206, "prime fetch is 206");
  Assert.equal(handlerHits, 1, "prime hit network once");

  await waitForWriteThrough();

  let suffix = await requestRange(url, "bytes=-2048");
  Assert.equal(suffix.code, 206, "suffix served as 206");
  Assert.equal(
    suffix.data,
    fullBody.substring(2048, 4096),
    "suffix bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "multi-region suffix served from cache via :sparsemeta resolve"
  );
});

// A lying server returns Content-Range `bytes 0-99/4096` for every request,
// including ours for `bytes=100-199`. The strict validator must reject the
// store: a follow-up request for the unsolicited 0-99 range AND a follow-up
// for the originally requested 100-199 range both go to network (the cache
// was neither poisoned with someone else's bytes at offset 100 nor with
// unsolicited bytes at offset 0).
add_task(async function test_sparse_strict_singleregion_rejects_wrong_range() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/lying_wrong_range`;
  handlerHits = 0;

  let r1 = await requestRange(url, "bytes=100-199");
  Assert.equal(r1.code, 206, "lying-server returns 206");
  Assert.equal(handlerHits, 1);

  await requestRange(url, "bytes=100-199");
  Assert.equal(
    handlerHits,
    2,
    "wrong-range response was not cached; second request hit network"
  );

  await requestRange(url, "bytes=0-99");
  Assert.equal(
    handlerHits,
    3,
    "unsolicited bytes from the lying server were not stored at offset 0"
  );
});

// Asking past EOF and getting clamped to the entity tail (server returns
// bytes 3500-4095/4096 to our `bytes=3500-99999`) is legitimate clamp-to-EOF.
// The cache must accept this and a follow-up bounded request for the clamped
// range serves from cache. Mirrors Chromium's partial_data.cc:346-355 carve-out.
add_task(async function test_sparse_strict_singleregion_accepts_eof_clamp() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/clamp_eof`;
  handlerHits = 0;

  let r1 = await requestRange(url, "bytes=3500-99999");
  Assert.equal(r1.code, 206, "clamp-to-EOF response is 206");
  Assert.equal(handlerHits, 1);

  await waitForWriteThrough();

  let r2 = await requestRange(url, "bytes=3500-4095");
  Assert.equal(r2.code, 206, "follow-up bounded range is 206");
  Assert.equal(
    r2.data,
    fullBody.substring(3500, 4096),
    "clamp-to-EOF bytes correct"
  );
  Assert.equal(
    handlerHits,
    1,
    "clamped-to-EOF response was cached and served on the second request"
  );
});

// A server that returns 416 for a sparse request dooms the in-flight region
// entry: a future bounded request for the same range refetches from network.
add_task(async function test_sparse_strict_416_dooms_entry() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
  let url = `http://localhost:${httpserver.identity.primaryPort}/always_416`;
  handlerHits = 0;

  let r1 = await requestRange(url, "bytes=500-999");
  Assert.equal(r1.code, 416, "server returned 416");
  Assert.equal(handlerHits, 1);

  await requestRange(url, "bytes=500-999");
  Assert.equal(
    handlerHits,
    2,
    "416 doomed the region; second request hit network"
  );
});

// A multipart/byteranges response with one unsolicited part: the requested
// parts cache normally; the unsolicited part is dropped. Follow-up requests
// for the unsolicited range go to network; follow-ups for the solicited
// ranges serve from cache.
add_task(
  async function test_sparse_strict_multirange_drops_unsolicited_parts() {
    Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 4096);
    let url = `http://localhost:${httpserver.identity.primaryPort}/lying_extra_multirange`;
    handlerHits = 0;

    let r1 = await requestRange(url, "bytes=100-199,500-599");
    Assert.equal(r1.code, 206);
    Assert.equal(handlerHits, 1);

    await waitForWriteThrough();

    // Bytes 2000-2099 (the unsolicited part) were dropped; this goes to network.
    await requestRange(url, "bytes=2000-2099");
    Assert.equal(
      handlerHits,
      2,
      "unsolicited multipart part was dropped; new request hit network"
    );
  }
);

// Concurrent bounded sub-range writes to the same region: cache2 allows only
// one open output stream per CacheFile at a time, so when many parallel
// requests target the same region entry, the losers used to silently fail
// (OpenOutputStream returns NS_ERROR_NOT_AVAILABLE → "entry doomed, not
// writing it"). The fix routes those losers through the capture-and-
// write-through path so their bytes still land via SparseMultiRangePartWriter
// (whose AsyncOpenURI serializes naturally after the winning writer
// releases the entry). Verify that every byte from the parallel batch is
// readable on the next visit.
add_task(async function test_concurrent_writes_same_region_all_persist() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_concurrent_same_region`;
  handlerHits = 0;

  // Eight disjoint, non-overlapping sub-ranges within region 0 (bytes 0-1023).
  // Fired in parallel; cache2 lets only one through to the output stream and
  // the rest take the fallback capture path.
  let ranges = [
    "bytes=0-63",
    "bytes=64-127",
    "bytes=128-255",
    "bytes=256-383",
    "bytes=384-511",
    "bytes=512-639",
    "bytes=640-767",
    "bytes=768-895",
  ];
  let firstPass = await Promise.all(ranges.map(r => requestRange(url, r)));
  for (let i = 0; i < ranges.length; i++) {
    Assert.equal(firstPass[i].code, 206, `prime ${ranges[i]} is 206`);
  }
  Assert.equal(
    handlerHits,
    ranges.length,
    "all primes hit the network exactly once"
  );

  // Give the deferred (capture-path) writes time to land.
  await waitForWriteThrough();

  // Second pass, sequential — every request must serve from cache.
  for (let r of ranges) {
    let res = await requestRange(url, r);
    Assert.equal(res.code, 206, `${r} cache hit is 206`);
    let [s, e] = r.replace("bytes=", "").split("-").map(Number);
    Assert.equal(res.data, fullBody.substring(s, e + 1), `${r} bytes correct`);
  }
  Assert.equal(
    handlerHits,
    ranges.length,
    "no extra network requests on the second pass"
  );
});

// Same shape but parallel requests target different regions of the same
// resource. Each region entry has its own CacheFile, so there's no
// per-entry writer contention — but the multi-region miss + capture path
// must still write every region without dropping any.
add_task(async function test_concurrent_writes_different_regions_all_persist() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_concurrent_multi_region`;
  handlerHits = 0;

  // Four bounded sub-ranges, each in its own region (R0..R3) of the
  // 4096-byte test resource (chunk_size=1024).
  let ranges = [];
  for (let r = 0; r < 4; r++) {
    let s = r * 1024 + 100;
    ranges.push(`bytes=${s}-${s + 63}`);
  }
  let firstPass = await Promise.all(ranges.map(r => requestRange(url, r)));
  for (let i = 0; i < ranges.length; i++) {
    Assert.equal(firstPass[i].code, 206, `prime ${ranges[i]} is 206`);
  }
  Assert.equal(handlerHits, ranges.length, "all primes hit the network");

  // Same write-through delay as the other tests. Each region has its own
  // CacheFile here so there's no per-entry writer contention; this catches
  // anything else (e.g. async commit racing the second-pass reads).
  await waitForWriteThrough();

  for (let r of ranges) {
    let res = await requestRange(url, r);
    Assert.equal(res.code, 206, `${r} cache hit is 206`);
    let [s, e] = r.replace("bytes=", "").split("-").map(Number);
    Assert.equal(res.data, fullBody.substring(s, e + 1), `${r} bytes correct`);
  }
  Assert.equal(
    handlerHits,
    ranges.length,
    "no extra network requests on the second pass"
  );
});

// Two parallel multi-range requests that BOTH touch the same regions. Each
// response spawns its own SparseMultiRangePartWriter, and without the
// per-region write queue both writers race for the single-output-stream slot
// on each shared CacheFile — the loser's bytes get silently dropped. With
// the queue, the writers take turns and every byte from both responses
// lands. Verifying both bodies are fully readable from cache on the next
// visit catches a regression in the SparseWriteQueue path.
add_task(async function test_concurrent_multirange_writers_serialize() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_concurrent_multirange`;
  handlerHits = 0;

  // Both multi-range requests target regions R0 + R1. Their sub-ranges
  // don't overlap in absolute bytes, but they DO compete for the same two
  // CacheFile output streams during the per-part write-through phase.
  let reqA = "bytes=100-199,1100-1199"; // R0:100-199 + R1:76-175
  let reqB = "bytes=300-399,1300-1399"; // R0:300-399 + R1:276-375
  let [a, b] = await Promise.all([
    requestRange(url, reqA),
    requestRange(url, reqB),
  ]);
  Assert.equal(a.code, 206, "request A is 206");
  Assert.equal(b.code, 206, "request B is 206");
  Assert.equal(handlerHits, 2, "both primes hit the network");

  await waitForWriteThrough();

  // Each sub-range from both responses must serve from cache. If the queue
  // didn't serialize, at least one of B's writes lost the race against A's
  // and one of these requests would re-fetch from network.
  let allRanges = [
    "bytes=100-199",
    "bytes=300-399",
    "bytes=1100-1199",
    "bytes=1300-1399",
  ];
  for (let r of allRanges) {
    let res = await requestRange(url, r);
    Assert.equal(res.code, 206, `${r} cache hit is 206`);
    let [s, e] = r.replace("bytes=", "").split("-").map(Number);
    Assert.equal(res.data, fullBody.substring(s, e + 1), `${r} bytes correct`);
  }
  Assert.equal(
    handlerHits,
    2,
    "every sub-range from both parallel multi-range responses was cached"
  );
});

// Reproduces a real bug found in production logs: when a multi-range
// response writes through to a sparse region in an order that puts a
// HIGHER CacheFile chunk before a LOWER one, the second (lower-chunk)
// write triggers GetChunkLocked to load the lower chunk from disk -- but
// the new region entry's on-disk file hasn't been created yet (its file
// handle is still being opened on the cache IO thread), so ReadInternal
// returns NS_ERROR_NOT_AVAILABLE. That error poisons the entire CacheFile
// status and ALL subsequent writes to the region silently fail.
//
// To force the race we make many concurrent multi-range write-throughs
// (each to its own region URL). The cache IO thread becomes saturated
// trying to open file handles for the new entries, while the main
// thread keeps writing into the in-memory chunk buffers. When part 1
// (the high CacheFile chunk) finishes and part 2 (the low chunk)
// starts, mHandle is often still null because the region entry's
// OpenFile is still queued. CacheFile::DeactivateChunk takes the
// !mOpeningFile=false branch so the dirty high chunk stays cached in
// memory rather than being flushed to disk, and the low-chunk write
// then hits the off < mDataSize branch in GetChunkLocked, attempts a
// disk read of a chunk that was never written, and fails.
//
// The bug shows up as cache misses on the replay requests: with the
// region entry's status poisoned to NS_ERROR_FILE_NOT_FOUND, every
// subsequent write to the region is silently dropped.
add_task(async function test_sparse_write_across_cache_chunks_in_reverse() {
  // 1 MB sparse region so we can address byte offsets up to ~1 MB and
  // span multiple CacheFile chunks (256 KB each).
  Services.prefs.setIntPref(
    "network.http.sparse_entries.chunk_size",
    1024 * 1024
  );
  handlerHits = 0;

  // Issue many concurrent multi-range fetches to distinct URLs so each
  // creates a brand-new region entry. The IO thread becomes busy
  // opening all of them at once, which keeps mHandle null on at least
  // one of them while its two part writes execute back-to-back.
  // Distinct URLs (varied by query string) so each fetch creates a
  // brand-new region entry; the handler routes all of them to the same
  // multipart-byteranges-capable body.
  let urls = [];
  for (let i = 0; i < 8; i++) {
    urls.push(
      `http://localhost:${httpserver.identity.primaryPort}/cog_cross_chunk_order?n=${i}`
    );
  }
  let req = "bytes=786432-808447,53248-73727";
  let firstPass = await Promise.all(urls.map(u => requestRange(u, req)));
  for (let r of firstPass) {
    Assert.equal(r.code, 206, "multi-range request is 206");
  }
  Assert.equal(handlerHits, urls.length, "each URL fetched once");

  await waitForWriteThrough();

  // Both sub-ranges from every URL should now serve from cache. The
  // bug shows up as one or more replays going to network because the
  // sparse writes were dropped.
  let highReplays = await Promise.all(
    urls.map(u => requestRange(u, "bytes=786432-808447"))
  );
  let lowReplays = await Promise.all(
    urls.map(u => requestRange(u, "bytes=53248-73727"))
  );
  for (let r of highReplays.concat(lowReplays)) {
    Assert.equal(r.code, 206, "replay request is 206");
  }

  Assert.equal(
    handlerHits,
    urls.length,
    "all sub-ranges from all URLs served from cache; no extra network " +
      "hits (if this fails, the cross-chunk write-order bug is reproduced)"
  );
});

// A multi-range request whose sub-ranges straddle region boundaries must
// still serve from cache: each spanning sub-range is split across regions
// and the multipart/byteranges body re-groups the per-region slices back
// into one multipart part per requested sub-range.
add_task(async function test_multirange_with_spanning_subrange_from_cache() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog15`;
  handlerHits = 0;

  // Populate the entire data span used by the test (regions 0 and 1):
  // bytes=0-2047 is a single multi-region miss that write-throughs to both
  // region 0 and region 1, then a follow-up multi-range with a spanning
  // sub-range should serve from cache (not from network).
  let warm = await requestRange(url, "bytes=0-2047");
  Assert.equal(warm.code, 206);
  Assert.equal(handlerHits, 1, "warmup is a single multi-region fetch");

  // Wait for the write-through to land before the cache-served request.
  await waitForWriteThrough();

  // Multi-range: first sub-range fits entirely in region 0; second sub-range
  // straddles region 0 -> region 1; third sub-range fits in region 1.
  let r = await requestRange(url, "bytes=200-299,900-1199,1500-1799");
  Assert.equal(r.code, 206, "multi-range with spanning sub-range is 206");
  Assert.ok(
    r.contentType && r.contentType.startsWith("multipart/byteranges"),
    "served as multipart/byteranges"
  );
  Assert.equal(handlerHits, 1, "served from cache; no additional network hit");
  // Spot-check the part bodies are in there.
  Assert.ok(r.data.includes(fullBody.substring(200, 300)));
  Assert.ok(r.data.includes(fullBody.substring(900, 1200)));
  Assert.ok(r.data.includes(fullBody.substring(1500, 1800)));
  Assert.ok(
    r.data.includes("Content-Range: bytes 900-1199/4096"),
    "spanning part has its own Content-Range"
  );
});

// Two regions populated with different validators (resource rotated between
// the fetches): the multi-region serve must NOT splice them — it falls back
// to the network for the full span.
add_task(async function test_multiregion_validator_mismatch() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/rotating_etag`;
  handlerHits = 0;
  etagCounter = 0;

  await requestRange(url, "bytes=500-1023");
  await requestRange(url, "bytes=1024-1500");
  Assert.equal(handlerHits, 2, "two single-region writes happened");

  let span = await requestRange(url, "bytes=500-1500");
  Assert.equal(span.code, 206);
  Assert.equal(
    span.data,
    fullBody.substring(500, 1501),
    "span bytes correct (from network)"
  );
  Assert.equal(
    handlerHits,
    3,
    "validator mismatch -> multi-region serve fell back to network"
  );
});

// SparseMetaResolver TTLs the not-sparse sentinel so a server that later
// adds a strong validator gets retried. We populate a sentinel by
// fetching a non-sparse-cacheable response (Content-Encoding: gzip), then
// reach into the cache to force the sentinel's expiration into the past,
// and finally issue a suffix range request -- SparseMetaResolver's
// OnCacheEntryCheck must return ENTRY_NOT_WANTED and let the channel
// fall back to a fresh network fetch.
add_task(async function test_sparsemeta_expired_sentinel_falls_back() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/content_encoded`;
  handlerHits = 0;

  // First request: response is gzip-encoded → not sparse-cacheable →
  // SparseMultiRangeCaptureListener marks the URL not-sparse with a
  // 24-hour sentinel TTL.
  let r1 = await requestRange(url, "bytes=0-99");
  Assert.equal(r1.code, 206);
  Assert.equal(handlerHits, 1);
  await waitForWriteThrough();

  // Reach into the sparsemeta entry and set its expiration time to 1
  // (epoch second 1 is well in the past), forcing the next
  // SparseMetaResolver::OnCacheEntryCheck to take the expired branch.
  let metaURI = Services.io.newURI(url);
  await new Promise(resolve => {
    Services.cache2
      .diskCacheStorage(Services.loadContextInfo.default, false)
      .asyncOpenURI(metaURI, ":sparsemeta", Ci.nsICacheStorage.OPEN_READONLY, {
        onCacheEntryCheck() {
          return Ci.nsICacheEntryOpenCallback.ENTRY_WANTED;
        },
        onCacheEntryAvailable(entry) {
          if (entry) {
            entry.setExpirationTime(1);
          } else {
            Assert.ok(false, "sparsemeta entry not found for expiry test");
          }
          resolve();
        },
        QueryInterface: ChromeUtils.generateQI(["nsICacheEntryOpenCallback"]),
      });
  });

  // Suffix request triggers SparseMetaResolver. Sentinel is expired, so
  // resolver returns ENTRY_NOT_WANTED and the channel goes to network.
  let r2 = await requestRange(url, "bytes=-50");
  Assert.equal(r2.code, 206);
  Assert.equal(
    handlerHits,
    2,
    "expired sentinel -> suffix request refetches from network"
  );
});

// Open-ended request "bytes=N-" with N >= the entity total. The
// SparseMetaResolver's OnCacheEntryAvailable hits the
// `if (mParam >= record.mTotal)` branch, calls OnSparseMetaUnresolved,
// and the channel falls back to a network fetch.
add_task(async function test_sparsemeta_open_ended_beyond_total_falls_back() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_open_ended_beyond`;
  handlerHits = 0;

  // First request populates the sparsemeta entry with total=4096.
  let prime = await requestRange(url, "bytes=0-99");
  Assert.equal(prime.code, 206);
  Assert.equal(handlerHits, 1);

  await waitForWriteThrough();

  // Open-ended request beyond total: mParam=999999 >= mTotal=4096.
  // The resolver bails out, channel falls back to network. The server
  // returns 416 (range not satisfiable) for any N >= total, so we just
  // verify the network was contacted.
  await requestRange(url, "bytes=999999-");
  Assert.equal(
    handlerHits,
    2,
    "open-ended N >= total: resolver bailed, request went to network"
  );
});

// SparseRegionReader rejects an expired region entry. Populate two
// regions, then mark the first region's entry expired via the cache
// API, then issue a multi-region span: the assembly fails, channel
// falls back to a fresh network fetch.
add_task(async function test_multiregion_expired_region_entry_falls_back() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_expired_region`;
  handlerHits = 0;

  // Two single-region writes populate region 0 and region 1.
  await requestRange(url, "bytes=500-1023");
  await requestRange(url, "bytes=1024-1500");
  Assert.equal(handlerHits, 2);

  await waitForWriteThrough();

  // Reach into region 0's entry and force its expiration into the past.
  // SparseRegionReader's OnCacheEntryAvailable will see expirationTime
  // <= now and call Fail().
  let regionURI = Services.io.newURI(url);
  await new Promise(resolve => {
    Services.cache2
      .diskCacheStorage(Services.loadContextInfo.default, false)
      .asyncOpenURI(
        regionURI,
        ":sparsechunk=0",
        Ci.nsICacheStorage.OPEN_READONLY,
        {
          onCacheEntryCheck() {
            return Ci.nsICacheEntryOpenCallback.ENTRY_WANTED;
          },
          onCacheEntryAvailable(entry) {
            if (entry) {
              entry.setExpirationTime(1);
            } else {
              Assert.ok(false, "region 0 sparsechunk entry not found");
            }
            resolve();
          },
          QueryInterface: ChromeUtils.generateQI(["nsICacheEntryOpenCallback"]),
        }
      );
  });

  // Multi-region span: SparseRegionReader hits the expired region,
  // calls Fail(), channel refetches.
  let span = await requestRange(url, "bytes=500-1500");
  Assert.equal(span.code, 206);
  Assert.equal(
    handlerHits,
    3,
    "expired region entry -> multi-region assembly failed, refetched"
  );
});

// First-time sparsemeta write to a brand-new URL: SparseMeta::Write sees
// no existing validator AND the caller-supplied Record has mRev=0
// (default), so the "fresh meta, rev defaults to 1" branch fires.
add_task(async function test_sparsemeta_first_write_sets_rev_one() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/cog_brand_new`;
  handlerHits = 0;

  // Any sparse-cacheable response triggers MaybeWriteSparseMeta with a
  // fresh Record(mRev=0). Since this URL is brand new, there's no
  // existing validator stamped on the sparsemeta entry, so the
  // `if (existingValidator.IsEmpty())` branch fires and inside it the
  // `if (rev == 0)` branch sets rev to 1.
  let r = await requestRange(url, "bytes=0-99");
  Assert.equal(r.code, 206);
  Assert.equal(handlerHits, 1);

  await waitForWriteThrough();

  // Verify the sparsemeta entry was actually written with a non-empty
  // rev so we know we exercised the path (not just attempted to).
  let metaURI = Services.io.newURI(url);
  await new Promise(resolve => {
    Services.cache2
      .diskCacheStorage(Services.loadContextInfo.default, false)
      .asyncOpenURI(metaURI, ":sparsemeta", Ci.nsICacheStorage.OPEN_READONLY, {
        onCacheEntryCheck() {
          return Ci.nsICacheEntryOpenCallback.ENTRY_WANTED;
        },
        onCacheEntryAvailable(entry) {
          if (!entry) {
            Assert.ok(false, "sparsemeta entry not written for brand-new URL");
            resolve();
            return;
          }
          let rev = entry.getMetaDataElement("sparsemeta-rev");
          Assert.equal(rev, "1", "first-time write sets rev=1");
          resolve();
        },
        QueryInterface: ChromeUtils.generateQI(["nsICacheEntryOpenCallback"]),
      });
  });
});

// Single-range responses are streamed through SparseSingleRangeCaptureListener
// to be replayed into the sparse cache after the response completes.
// The capture buffer is capped at 16 MiB; oversize responses set
// mCappedOut=true, drop the captured buffer, and skip write-through.
// We exercise that path with a 17 MiB body and then verify a follow-up
// small-range request goes to network (no sparse data was stored).
add_task(
  async function test_single_range_oversize_capture_drops_writethrough() {
    Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
    let url = `http://localhost:${httpserver.identity.primaryPort}/huge_single`;
    handlerHits = 0;

    // Request the whole 17 MiB body. SparseSingleRangeCaptureListener buffers
    // it; passes the 16 MiB cap; mCappedOut=true; write-through dropped.
    let big = await requestRange(url, `bytes=0-${HUGE_TOTAL - 1}`);
    Assert.equal(big.code, 206, "oversize single-range response is 206");
    Assert.equal(
      big.data.length,
      HUGE_TOTAL,
      "full body delivered to consumer"
    );
    Assert.equal(handlerHits, 1);

    await waitForWriteThrough();

    // No sparse-cache data was stored (write-through was dropped on oversize),
    // so a follow-up small-range request must go to network.
    let small = await requestRange(url, "bytes=0-99");
    Assert.equal(small.code, 206);
    Assert.equal(
      handlerHits,
      2,
      "oversize capture dropped write-through -> follow-up refetches from network"
    );
  }
);

// Same as the single-range case, but exercises SparseMultiRangeCaptureListener:
// a multipart/byteranges body whose total framed size exceeds 16 MiB.
add_task(async function test_multi_range_oversize_capture_drops_writethrough() {
  Services.prefs.setIntPref("network.http.sparse_entries.chunk_size", 1024);
  let url = `http://localhost:${httpserver.identity.primaryPort}/huge_multi`;
  handlerHits = 0;

  // Two ~9 MiB parts -- combined ~18 MiB > 16 MiB cap.
  let nineM = 9 * 1024 * 1024;
  let req = `bytes=0-${nineM - 1},${nineM}-${HUGE_TOTAL - 1}`;
  let r = await requestRange(url, req);
  Assert.equal(r.code, 206, "oversize multi-range response is 206");
  Assert.equal(handlerHits, 1);

  await waitForWriteThrough();

  // No sparse-cache data stored, so a follow-up small range goes to network.
  let small = await requestRange(url, "bytes=0-99");
  Assert.equal(small.code, 206);
  Assert.equal(
    handlerHits,
    2,
    "oversize multipart capture dropped write-through -> follow-up refetches"
  );
});
