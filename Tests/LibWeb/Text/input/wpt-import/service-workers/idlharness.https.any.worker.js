
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../resources/testharness.js");
importScripts("../resources/WebIDLParser.js")
importScripts("../resources/idlharness.js")
importScripts("cache-storage/resources/test-helpers.js")
importScripts("service-worker/resources/test-helpers.sub.js")
importScripts("../service-workers/idlharness.https.any.js");
done();
