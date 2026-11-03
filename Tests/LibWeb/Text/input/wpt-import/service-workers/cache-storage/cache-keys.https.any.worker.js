
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../../resources/testharness.js");
self.META_TITLE = "Cache.keys";
importScripts("./resources/test-helpers.js")
importScripts("../../service-workers/cache-storage/cache-keys.https.any.js");
done();
