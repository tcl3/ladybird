
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../../resources/testharness.js");
self.META_TITLE = "Cache.matchAll";
importScripts("./resources/test-helpers.js")
importScripts("../../service-workers/cache-storage/cache-matchAll.https.any.js");
done();
