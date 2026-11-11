// The player webview's script: one <audio> element playing what AudioService asks for, from
// StreamCache's loopback URLs. Native calls window.__ncmAudio([command, ...args]); the page
// answers [event, source, value] through saucer's message port.
(function () {
  "use strict";

  if (location.protocol !== "http:" || location.pathname !== "/player") return;

  var post = window.saucer.internal.message;
  var stallMs = 500;
  var current = null; // the open track: {element, source, loaded, time, movedAt}

  function report(track, event, value) {
    if (track === current) post(JSON.stringify([event, track.source, value]));
  }

  function bufferedEnd(element) {
    var ranges = element.buffered, time = element.currentTime;
    for (var i = 0; i < ranges.length; ++i)
      if (ranges.start(i) <= time && time <= ranges.end(i)) return ranges.end(i);
    return time;
  }

  var commands = {
    open: function (source, start, level) {
      commands.close();
      var element = new Audio();
      var track = current = {element: element, source: source, loaded: false, time: 0, movedAt: 0};
      element.preload = "auto";
      // WebKit restores the previous stream's volume unless the element's volume was
      // changed explicitly. Set it before assigning the source, even at the default level.
      if (element.volume === level) element.volume = 0.5;
      element.volume = level;
      element.addEventListener("loadedmetadata", function () {
        if (track !== current) return;
        if (start > 0) element.currentTime = start;
        track.loaded = true;
        commands.sample();
      }, {once: true});
      element.addEventListener("ended", function () { report(track, "ended"); });
      element.addEventListener("error", function () { report(track, "error"); });
      element.src = source;
    },

    close: function () {
      if (!current) return;
      var element = current.element;
      current = null;
      element.pause();
      element.removeAttribute("src");
      element.load(); // drops the connection
    },

    play: function (level) {
      var track = current;
      track.element.volume = level;
      track.movedAt = performance.now();
      track.element.play().catch(function (error) {
        if (error.name !== "AbortError") report(track, "error");
      });
    },

    pause: function () {
      current.element.pause();
    },

    seek: function (position) {
      current.movedAt = performance.now();
      current.element.currentTime = position;
    },

    volume: function (level) {
      current.element.volume = level;
    },

    // Waiting is determined by the position: WebKit's readyState can stay
    // below HAVE_FUTURE_DATA after a seek while the track plays on.
    sample: function () {
      var track = current, element = track.element, now = performance.now();
      if (element.currentTime !== track.time) {
        track.time = element.currentTime;
        track.movedAt = now;
      }
      report(track, "sample", {
        loaded: track.loaded,
        duration: isFinite(element.duration) ? element.duration : 0,
        position: element.currentTime,
        buffered: bufferedEnd(element),
        paused: element.paused,
        waiting: !element.paused && now - track.movedAt > stallMs
      });
    }
  };

  window.__ncmAudio = function (command) {
    if (current || command[0] === "open") commands[command[0]].apply(null, command.slice(1));
  };
  post(JSON.stringify(["ready"]));
})();
