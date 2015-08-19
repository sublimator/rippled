require('babel/register');

var extend = require('extend');
var mocha = require('mocha');

require('./ripplelib-legacy-support')(require('ripple-lib'));

// Optionally use a more useful (but noisy) logger
if (process.env.USE_RCONSOLE) {
  require('rconsole');
};

var oldLoader = mocha.prototype.loadFiles
if (!oldLoader.monkeyPatched) {
  // Gee thanks Mocha ...
  mocha.prototype.loadFiles = function() {
    try {
      oldLoader.apply(this, arguments);
    } catch (e) {
      // Normally mocha just silently bails
      console.error(e.stack);
      // We throw, so mocha doesn't continue trying to run the test suite
      throw e;
    }
  }
  mocha.prototype.loadFiles.monkeyPatched = true;
};

