
"use strict";

console.log("START channel.js");

new QWebChannel(qt.webChannelTransport, function (webChannel) {
        console.log("web channel initialized:" + window.location.href);
        window.nativeChannel = webChannel.objects.channel;
        console.log("nativeChannel:", window.nativeChannel);

        channel.init();
    }
);
