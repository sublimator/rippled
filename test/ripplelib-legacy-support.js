'use strict';

const _ = require('lodash');

function getCallingPosition(traceIx) {
  var position = '';

  try {
    var trace = require('stack-trace');
    var callSite = trace.get()[traceIx + 1];
    position = ' (near: ' +
               callSite.getFunctionName() + ' @ ' +
               callSite.getFileName() +  ':' +
               callSite.getLineNumber() + ':' +
               callSite.getColumnNumber() +
               ') ';  /* padding ;) */
  } catch (e) {
    if(process /* on nodejs ;) */) {
      console.log('e', e);
    }
  }
  return position;
}

function restoreRequestConstructorPositionalArgs(ripplelib) {
  var Remote = ripplelib.Remote;

  /**
   *  Create options object from positional function arguments
   *
   * @param {Array} params function parameters
   * @param {Array} args function arguments
   * @return {Object} keyed options
   */

  function makeOptions(command, params, args) {
    const result = {};

    if (!_.get(process, 'env.SUPPRESS_DEPRECATION_WARNINGS')) {
      console.warn(
        'DEPRECATED: First argument to ' + command
        + ' request constructor must be an object containing'
        + ' request properties: '
        + params.join(', ')
        + getCallingPosition(2)
      );

    }

    if (_.isFunction(_.last(args))) {
      result.callback = args.pop();
    }

    return _.merge(result, _.zipObject(params, args));
  }

  function addBackwardsCompatibility(compatParams) {
    const {method,
           command,
           positionals = [],
           mappings = {},
           hasCallback = true,
           aliases = []} = compatParams;

    const needsWrapping = positionals.length ||
                          Object.keys(mappings).length;

    function wrapFunction(func) {
      return function() {
        const optionsArg = arguments[0];
        const options = {};

        if (hasCallback) {
          options.callback = arguments[1];
        }

        if (_.isPlainObject(optionsArg)) {
          const mapped = _.transform(optionsArg, (result, v, k) => {
            const to = mappings[k];
            result[to !== undefined ? to : k] = v;
          });
          _.merge(options, mapped);
        } else {
          const args = _.slice(arguments);
          const positionalOptions = makeOptions(command, positionals, args);
          _.merge(options, positionalOptions);
        }
        return func.call(this, options, options.callback);
      };
    }

    const obj = Remote.prototype;
    // Wrap the function and set the aliases
    const wrapped = needsWrapping ? wrapFunction(obj[method]) : obj[method];
    aliases.concat(method).forEach((name) => {
      obj[name] = wrapped;
    });
  }

  const remoteMethods = [
    {
      method: 'requestPathFindCreate',
      command: 'path_find',
      positionals: ['source_account',
                   'destination_account',
                   'destination_amount',
                   'source_currencies'],
      mappings: {
        src_currencies: 'source_currencies',
        src_account: 'source_account',
        dst_amount: 'destination_amount',
        dst_account: 'destination_account'
      }
    },
    {
      method: 'requestRipplePathFind',
      command: 'ripple_path_find',
      positionals: ['source_account',
                   'destination_account',
                   'destination_amount',
                   'source_currencies'],
      mappings: {
        src_currencies: 'source_currencies',
        src_account: 'source_account',
        dst_amount: 'destination_amount',
        dst_account: 'destination_account'
      }
    },
    {
      method: 'createPathFind',
      aliases: ['pathFind'],
      command: 'pathfind',
      positionals: ['src_account',
                   'dst_account',
                   'dst_amount',
                   'src_currencies']
    },
    {
      method: 'requestTransactionEntry',
      command: 'transaction_entry',
      positionals: ['hash', 'ledger'],
      mappings: {ledger_index: 'ledger', ledger_hash: 'ledger'}
    },
    {
      method: 'requestTransaction',
      command: 'tx',
      positionals: ['hash', 'ledger'],
      mappings: {ledger_index: 'ledger', ledger_hash: 'ledger'},
      aliases: ['requestTx']
    },
    {
      method: 'requestBookOffers',
      command: 'book_offers',
      positionals: ['gets', 'pays', 'taker', 'ledger', 'limit'],
      mappings: {taker_pays: 'pays', taker_gets: 'gets'}
    },
    {
      method: 'createOrderBook',
      hasCallback: false,
      command: 'orderbook',
      positionals: ['currency_gets', 'issuer_gets',
                   'currency_pays', 'issuer_pays']
    },
    {
      method: 'requestTransactionHistory',
      command: 'tx_history',
      positionals: ['start'],
      aliases: ['requestTxHistory']
    },
    {
      method: 'requestWalletAccounts',
      command: 'wallet_accounts',
      positionals: ['seed']
    },
    {
      method: 'requestSign',
      command: 'sign',
      positionals: ['secret', 'tx_json']
    },
    {
      method: 'accountSeqCache',
      command: 'accountseqcache',
      positionals: ['account', 'ledger']
    },
    {
      method: 'requestRippleBalance',
      command: 'ripplebalance',
      positionals: ['account', 'issuer', 'currency', 'ledger']
    },
    {
      method: 'requestAccountInfo',
      command: 'account_info',
      positionals: ['account', 'ledger', 'peer', 'limit', 'marker']
    },
    {
      method: 'requestAccountCurrencies',
      command: 'account_currencies',
      positionals: ['account', 'ledger', 'peer', 'limit', 'marker']
    },
    {
      method: 'requestAccountLines',
      command: 'account_lines',
      positionals: ['account', 'peer', 'ledger', 'limit', 'marker']
    },
    {
      method: 'requestAccountOffers',
      command: 'account_offers',
      positionals: ['account', 'ledger', 'peer', 'limit', 'marker']
    },

    {
      method: 'requestAccountBalance',
      command: 'account_balance',
      positionals: ['account', 'ledger']
    },
    {
      method: 'requestAccountFlags',
      command: 'account_flags',
      positionals: ['account', 'ledger']
    },
    {
      method: 'requestOwnerCount',
      command: 'owner_count',
      positionals: ['account', 'ledger']
    }
  ];

  remoteMethods.forEach(addBackwardsCompatibility);
}

function restoreSnakeCase(ripplelib) {
  // camelCase to under_scored API conversion
  function attachUnderscored(c) {
    var o = ripplelib[c];

    Object.keys(o.prototype).forEach(function(key) {
      var UPPERCASE = /([A-Z]{1})[a-z]+/g;

      if (!UPPERCASE.test(key)) {
        return;
      }

      var underscored = key.replace(UPPERCASE, function(c) {
        return '_' + c.toLowerCase();
      });

      o.prototype[underscored] = o.prototype[key];
    });
  };

  [ 'Remote',
    'Request',
    'Transaction',
    'Account',
    'Server'
  ].forEach(attachUnderscored);
}

function restoreConfig(ripplelib) {
  var config = ripplelib.config = {
    load: function (newOpts) {
      _.merge(ripplelib.config, newOpts);
      return config;
    }
  }

  var Remote = ripplelib.Remote;
  Remote.from_config = function(obj, trace) {
    var serverConfig = (typeof obj === 'string') ? config.servers[obj] : obj;
    var remote = new Remote(serverConfig, trace);

    function initializeAccount(account) {
      var accountInfo = config.accounts[account];
      if (typeof accountInfo === 'object') {
        if (accountInfo.secret) {
          // Index by nickname
          remote.setSecret(account, accountInfo.secret);
          // Index by account ID
          remote.setSecret(accountInfo.account, accountInfo.secret);
        }
      }
    };

    if (config.accounts) {
      Object.keys(config.accounts).forEach(initializeAccount);
    }

    return remote;
  };
}

function restoreAccountAliases(ripplelib) {
  var accountParse = ripplelib.UInt160.prototype.parse_json;
  ripplelib.UInt160.prototype.parse_json = function(j) {
    if (ripplelib.config.accounts[j]) {
      j = ripplelib.config.accounts[j].account;
    }
    return accountParse.call(this, j);
  };
}

function restoreXRPFloatingPointLiterals(ripplelib) {
  var amountParse = ripplelib.Amount.prototype.parse_json;
  ripplelib.Amount.prototype.parse_json = function(j) {
    if (typeof j === 'string'/* || typeof j === 'number'*/) {
      /*j = String(j);*/
      if (j.match(/^\s*\d+\.\d+\s*$/)) {
        j = String(Math.floor(parseFloat(j, 10) * 1e6));
      }
    }
    return amountParse.call(this, j);
  };
}

module.exports = function wrapAPI(ripplelib) {
  _.merge(ripplelib, ripplelib._DEPRECATED);
  restoreRequestConstructorPositionalArgs(ripplelib);
  restoreSnakeCase(ripplelib);
  restoreConfig(ripplelib);
  restoreAccountAliases(ripplelib);
  restoreXRPFloatingPointLiterals(ripplelib);
};
