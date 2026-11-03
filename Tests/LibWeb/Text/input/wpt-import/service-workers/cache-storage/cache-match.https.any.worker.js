
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../../resources/testharness.js");
self.META_TITLE = "Cache.match";
importScripts("./resources/test-helpers.js")
importScripts("../../common/get-host-info.sub.js")
importScripts("../../service-workers/cache-storage/cache-match.https.any.js");
done();
