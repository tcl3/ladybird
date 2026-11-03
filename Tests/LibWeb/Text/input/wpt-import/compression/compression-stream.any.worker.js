
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../resources/testharness.js");
importScripts("third_party/pako/pako_inflate.min.js")
importScripts("resources/decompress.js")
importScripts("resources/formats.js")
importScripts("../compression/compression-stream.any.js");
done();
