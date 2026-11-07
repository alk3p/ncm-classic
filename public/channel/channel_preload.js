
// Define global channel object thus scripts in web page can call methods
// defined in that object.
// But right now, qt web channel has not yet been initialized.

"use strict";

console.log("START channel_preload.js");

function sendHidePopupMenuEvent(event) {
    channel.call("winhelper.hidePopupMenu", function(){}, []);
}

var channel = {
    signalSlotMap: {
        "os.checkNativeSupportFonts": "os.onCheckNativeSupportFonts",
        "os.querySystemFonts": "os.onQuerySystemFonts",
        "storage.init": "storage.onInit"
    },

    noop: function() {},

    // Call a native function with |args| as parameter list.
    // And then call the js |callback| function.
    // The value returned from native function is used as params of |callback|.
    call: function (nativeFuncName, callback, args) {
        console.log("[channel].call() " + nativeFuncName + ", " +
            callback + ", " + args, args);

        if (!this.initialized) {
            this._appendCallTask(nativeFuncName, callback, args);
            return;
        }

        // A 'null' object is treated as an empty QVariantMap in Qt.
        // So we convert any "undefined" to an empty object.
        for (var i = 0; i < args.length; i++) {
            if (args[i] === undefined) {
                args[i] = {};
            }
        }

        // Remap signals and slots.
        if (nativeFuncName in this.signalSlotMap) {
            this.registerCall(this.signalSlotMap[nativeFuncName], callback);
            callback = this.noop();
        }

        // Remap function arguments.
        switch (nativeFuncName) {
            case "app.exit": {
                // Remap overloaded methods.
                if (args.length === 1) {
                    nativeFuncName = "app.exitWithMode";
                } else if (args.length === 4) {
                    nativeFuncName = "app.exitWithMove";
                }
                break;
            }
            case "app.selectSystemDir": {
                // Convert last argument from string array to string.
                if (args.length === 4) {
                    if (Array.isArray(args[3])) {
                        args[3] = (args[3].length > 0) ? args[3][0] : "";
                    }
                }
                break;
            }
            case "musiclibrary.execSql": {
                // Remap overloaded methods.
                if (args.length === 2 && Array.isArray(args[1])) {
                    nativeFuncName = "musiclibrary.execSqls";
                }
                break;
            }
            case "os.checkNativeSupportFonts": {
                // os.checkNativeSupportFonts() expect only one argument.
                args = [args];
                break;
            }
            case "os.getDiskSpace": {
                // Remap overloaded methods.
                // os.getDiskSpace() expect only one argument.
                if (args.length === 2) {
                    args = [args[0]];
                }
                break;
            }
            case "player.setFavour": {
                // Convert string to int.
                args = args.map(parseInt);
                break;
            }
            case "storage.clearCache": {
                // bogus invoke
                if (args.length === 1 && args[0] === null) {
                    args = [[]];
                }
                break;
            }
            case "winhelper.hidePopupMenu": {
                document.removeEventListener("click", sendHidePopupMenuEvent, true);
                break;
            }
            case "winhelper.popupMenu": {
                // Inject hooks.
                setTimeout(function() {
                    document.addEventListener("click", sendHidePopupMenuEvent, true);
                }, 0);
                break;
            }
            case "winhelper.setNativeWindowShow": {
                // Remap overload function in C++, as web channel does not
                // support method overloading.
                if (args.length !== 0) {
                    nativeFuncName = "winhelper.setNativeSubWindowShow";
                }
                break;
            }
            default: {}
        }

        // NOTE(xushaohua): To bypass qobject error:
        // "Error: cannot call function of deleted QObject".
        var objectList = nativeFuncName.split(".");
        if (objectList && objectList.length === 2) {
            var nativeObject = objectList[0];
            var nativeMethod = objectList[1];
            var func = window.nativeChannel &&
                window.nativeChannel[nativeObject] &&
                window.nativeChannel[nativeObject][nativeMethod];
            if (func) {
                try {
                    // Append callback to argument list.
                    if (! (nativeFuncName in this.signalSlotMap)) {
                        args.push(callback);
                    }
                    func.apply(null, args);
                } catch (err) {
                    console.error("Failed to call slot: ", nativeFuncName, err);
                }
            } else {
                console.error("TODO: Provide this native slot:", nativeFuncName);
            }
        } else {
            console.error("TODO: invalid native slot callback:", nativeFuncName);
        }
    },

    // Connect signals in QObjects to |callback|.
    // |nativeFuncName| is the signal name of a QObject.
    registerCall: function (nativeFuncName, callback) {
        console.log("[channel].registerCall(): " + nativeFuncName + ", " +  callback);

        if (!this.initialized) {
            this._appendCallbackTask(nativeFuncName, callback);
            return;
        }

        var objectList = nativeFuncName.split(".");
        if (objectList && objectList.length === 2) {
            var nativeObject = objectList[0];
            var nativeSignal = objectList[1];
            if (window.nativeChannel &&
                nativeChannel[nativeObject] &&
                nativeChannel[nativeObject][nativeSignal]) {
                try {
                    nativeChannel[nativeObject][nativeSignal].connect(callback);
                } catch (err) {
                    console.error("Failed to connect slots:", err);
                }
            } else {
                console.error("TODO: Provide this native signal:", nativeFuncName,
                    window.nativeChannel, nativeObject, nativeSignal);
            }
        } else {
            console.error("TODO: Invalid native signal:", nativeFuncName);
        }
    },

    // Initialize internal objects.
    init: function () {
        console.log("[channel].init()");
        if (this.initialized) {
            console.warn("Channel already initialized");
            return;
        }

        // Expose native encryption interface.
        channel.encodeAnonymousId = encodeAnonymousId;
        channel.encodeAnonymousId2 = encodeAnonymousId;
        channel.encryptId = encryptId;
        channel.serialData = serialData;
        channel.serialData2 = serialData;

        this.initialized = true;

        // Connect signals to callback first.
        this._consumeCallbackTasks();

        this._consumeCallTasks();
    },

    initialized: false,

    // Put call() tasks in queue if qt web channel has not yet been fully
    // initialized.
    callTaskList: [],
    _appendCallTask: function(nativeFuncName, callback, args) {
        console.log("_appendCallTask()", nativeFuncName, callback, args);
        this.callTaskList.push([nativeFuncName, callback, args]);
    },
    _consumeCallTasks: function() {
        var that = this;
        this.callTaskList.forEach(function(params) {
            that.call.apply(that, params);
        });
    },

    // Put registerCall() tasks in queue if qt web channel has not yet been
    // fully initialized.
    callbackTaskList: [],
    _appendCallbackTask: function(nativeFuncName, callback) {
        console.log("_appendCallbackTask()", nativeFuncName, callback);
        this.callbackTaskList.push([nativeFuncName, callback]);
    },
    _consumeCallbackTasks: function() {
        var that = this;
        this.callbackTaskList.forEach(function(params) {
            that.registerCall.apply(that, params);
        });
    },
};