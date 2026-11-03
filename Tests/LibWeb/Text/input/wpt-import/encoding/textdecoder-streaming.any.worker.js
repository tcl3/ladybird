
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../resources/testharness.js");
self.META_TITLE = "Encoding API: Streaming decode";
importScripts("resources/encodings.js")
importScripts("../common/sab.js")
importScripts("../encoding/textdecoder-streaming.any.js");
done();
