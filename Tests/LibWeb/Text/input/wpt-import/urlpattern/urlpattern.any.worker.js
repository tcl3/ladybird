
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("../resources/testharness.js");
importScripts("resources/urlpatterntests.js")
importScripts("../urlpattern/urlpattern.any.js");
done();
