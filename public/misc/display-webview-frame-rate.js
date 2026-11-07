// Displays web animation frame at top right cornder of page.
(function() {
  document.addEventListener("DOMContentLoaded", function() {
    var node = document.createElement("span");
    node.id = "fpsMeter";
    node.style.position = "fixed";
    node.style.top = 0;
    node.style.right = 0;
    node.style.zIndex = 10000;
    node.innerHTML = "FPS"
    document.body.appendChild(node);
    var counter = 0;
    var timeStart = 0;
    var onFrame = function(time) {
      var delta = time - timeStart;
      if (delta >= 200) {
        node.innerHTML = (counter / delta * 1000) | 0;
        timeStart = time;
        counter = 0;
      }
      counter++;
      requestAnimationFrame(onFrame);
    };
    requestAnimationFrame(onFrame);
  })
})();
