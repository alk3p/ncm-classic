/** Used to inject fullscreen video controller to webview **/

(function() {
  function noop() {}

  var addStyle = function(style) {
    console.log("addstyle:", style);
    var css = document.createElement("style");
    css.type = "text/css";
    css.innerHTML = style;
    document.body.appendChild(css);
  };

  var style = [
    ".fullscreen-video-container {",
      "display: inline-block;",
      "position: fixed;",
      "height: 100vh;",
      "width: 100vw;",
      "top: 0;",
      "z-index: 999999999999;",
      "left: 0;",
      "overflow: hidden;",
    "}",
    ".fullscreen-video {",
      "width: 100%;",
      "height: 100%;",
    "}",
    ].join("");
  var kFullscreenVideoContainerClass = "fullscreen-video-container";
  var kFullscreenVideoClass = "fullscreen-video";
  //Object.defineProperty(document, "webkitFullscreenElement",
  //                      {writeable: true, configurable: true, value: null});
  
  var enterFullscreen = function() {
    var playerWrapper = document.querySelector(".m-mvplayer");
    var video = playerWrapper.querySelector("video");
    var fullscreenButton = playerWrapper.querySelector(".fullscreen");

    document.webkitFullscreenElementFake = video;

    playerWrapper.classList.add(kFullscreenVideoContainerClass);
    video.classList.add(kFullscreenVideoClass);
    fullscreenButton.classList.add("z-full");

    var event = new Event("webkitfullscreenchange",
                          {"bubbles": true, "cancelable": true})
    playerWrapper.dispatchEvent(event);

    channel.call("winhelper.enterFullscreen", noop, []);
  };

  var exitFullscreen = function() {
    var playerWrapper = document.querySelector(".m-mvplayer");
    var video = playerWrapper.querySelector("video");
    var fullscreenButton = playerWrapper.querySelector(".fullscreen");

    document.webkitFullscreenElementFake = null;

    playerWrapper.classList.remove(kFullscreenVideoContainerClass);
    video.classList.remove(kFullscreenVideoClass);
    fullscreenButton.classList.remove("z-full");

    var event = new Event("webkitfullscreenchange",
                          {"bubbles": true, "cancelable": true})
    playerWrapper.dispatchEvent(event);

    channel.call("winhelper.exitFullscreen", noop, []);
  };

  // Rebind event handler.
  HTMLDivElement.prototype.webkitRequestFullScreen = enterFullscreen;
  HTMLDivElement.prototype.webkitRequestFullscreen = enterFullscreen;
  document.webkitCancelFullScreen = exitFullscreen;
  addStyle(style);
}());
