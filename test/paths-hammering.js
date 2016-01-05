/* -------------------------------- REQUIRES -------------------------------- */

var _ = require('lodash');
var async = require('async');
var assert = require('assert');
var Request = require('ripple-lib').Request;
var Remote = require('ripple-lib').Remote;
var Amount = require('ripple-lib').Amount;
var PathFind = require('ripple-lib')._test.PathFind;
var makeSuite = require('./declarative-suite').makeSuite;

/* -------------------------------- CONSTANTS ------------------------------- */

/*
A "manual" test to connect to an already spawned server
    rippled should be set up to log to /dev/null

Connect [R] many remotes

Request the account state:
    Group account roots by Account
    Find accounts that have positive balances
    Find accounts that have positive balance potential (balance < limit)
    Create path requests [reqs] as product of previous two sets LIMIT by [n]

For each remote:
    Claim a path request (pop from [reqs])
    Drop connection with configurable [fuq]rency
        upon reconnect, try to send path request again
    Resend request if no response with configurable [impatience]

TODO:
  clean this mess up

*/


 // Number of simultaneous websocket connections to make path finding requests on
var REMOTES = 50;
var MAX_REQUESTS = 1000; // maximum path requests to try
var CLOSE_LEDGERS = true; // every 2-4 seconds
var RETRY_AFTER = 1250; // retry after ms timeout without update. 0 for no retry
var WAIT_FULL_REPLY = true;
// 'close'|'dispose', close will path_find subcommand : close
var DISPOSE_OR_CLOSE = 'dispose';
var CONNECTION_DROP_FREQUENCY = 0.3; // 0 - 1.0
var CONNECTION_DROP_AFTER = 50; // ms
var CLOSE_FIRST = false;
var LEDGER_DUMP = 'ledger-full-' + '1000' + '000.json';
var START_OWN_SERVER = true;

/* ----------------------------- MONKEY BUSINESS ---------------------------- */

// Does exactly the same as `PathFind::close` except doesn't actually issue a
// {command: 'path_find', 'subcommand' : 'close'} request
PathFind.prototype.dispose = function () {
  this.removeAllListeners('update');
  this.remote._cur_path_find = null;
  this.emit('end');
  this.emit('close');
};

/* --------------------------------- HELPERS -------------------------------- */

function getLedger(remote, cb) {
  remote.request_ledger({validated: true, full: true}, function (e, m) {
    assert.ifError(e);
    cb(m.ledger);
  })
}

function prettyJSON(o) {
  return JSON.stringify(o, undefined, 2);
}

function isAccountRoot(e) {
  return e.LedgerEntryType === 'AccountRoot';
}

function isLine(e) {
  return e.LedgerEntryType === 'RippleState';
}

function negateValue(v) {
  if (v[0] === '-') {
    return v.slice(1);
  }
  if (v !== '0') {
    return '-' + v;
  }
  return v;
}

function getOrCreateArray(obj, name) {
  if (Array.isArray(obj[name])) {
    return obj[name];
  } else {
    return (obj[name] = []);
  }
}

function isPositive(value) {
  return value !== '0' && value[0] !== '-';
}

function potentialBalanceAdj(line) {
  if (isPositive(line.limit)) {
    var limit = Number(line.limit);
    var balance = Number(line.balance);
    return limit - balance;
  }
  return 0;
}

function groupAccounts(state) {
  var accounts = _.indexBy(state.filter(isAccountRoot), 'Account');
  var lines = state.filter(isLine);

  lines.forEach((l) => {
    var low = l.LowLimit.issuer;
    var high = l.HighLimit.issuer;
    var currency = l.Balance.currency;
    var balance = l.Balance.value;
    var lowLimit = l.LowLimit.value;
    var highLimit = l.HighLimit.value;

    var lowLines = getOrCreateArray(accounts[low], 'lines');
    var highLines = getOrCreateArray(accounts[high], 'lines');

    lowLines.push({limit: lowLimit,
                  currency,
                  balance,
                  counterparty: high});

    highLines.push({limit: highLimit,
                    currency,
                    balance: negateValue(balance),
                    counterparty: low});

  });
  var accountsWithBalance = _.transform(accounts, (r, v, k ) => {
    if (_.any(v.lines, (l) => isPositive(l.balance))) {
      r[k] = v;
    }
  });
  var accountsWithBalancePotential = _.transform(accounts, (r, v, k ) => {
    var lines = _.filter(v.lines, (l) => potentialBalanceAdj(l) > 0);
    if (lines.length > 0) {
      r[k] = lines;
    }
  });
  return {accounts, accountsWithBalance, accountsWithBalancePotential};
}

function findPathRequests(state) {
  var {accountsWithBalance, accountsWithBalancePotential, accounts} =
        groupAccounts(state);

  var requests = [];
  _.forOwn(accountsWithBalancePotential, (lines, dest) => {
    lines.forEach(line => {
      _.forOwn(accountsWithBalance, (srcAccount, srcID) => {
        if (requests.length < MAX_REQUESTS) {
          requests.push([srcID, dest, line, potentialBalanceAdj(line)]);
        } else {
          return false;
        }
      })
    });
  });
  return requests;
}

function makeRemotes(like, n, onDone) {
  var remotes = _.fill(Array(n), like._servers[0]._opts)
      .map((opt) => {
        return new Remote({servers: [_.clone(opt)]})
      });
  async.parallel(
    remotes.map(remote => (cb) => remote.connect(cb)),
    () => {
      onDone(remotes);
  });
}

function closeLedger(remote) {
  setTimeout(() => {
    remote.ledgerAccept((e, m) => {
      console.log('ledgerAccept', m);
      closeLedger(remote);
    });
  }, 2000 + (Math.random() * 2000))
}

makeSuite('path_find', {dump: LEDGER_DUMP, no_server: START_OWN_SERVER},
  {
    test1: function (remote, done) {
      this.timeout(0);
      getLedger(remote, (ledger) => {
        CLOSE_LEDGERS && closeLedger(remote);

        makeRemotes(remote, REMOTES, (remotes) => {
          var state = ledger.accountState;
          var requests = findPathRequests(state);
          var finished = 0;
          var outstanding = 0;

          function doOne(remote) {
            if (requests.length === 0) {
              if (outstanding === 0) {
                done();
              }
              return;
            }

            if (!remote.reconnectHandle &&
                 Math.random() > (1 - CONNECTION_DROP_FREQUENCY)) {
              remote.reconnectHandle = setTimeout((() => {
                setTimeout(() => {
                  var pf = remote._cur_path_find;
                  if (pf) {
                    pf[DISPOSE_OR_CLOSE]();
                  }
                  request();
                }, 100);
                remote.reconnect();
                remote.reconnectHandle = null;
              }), CONNECTION_DROP_AFTER);
            }

            var req = requests.pop();
            var [src, dest, line, potentialBalanceAdj] = req;

            var options = {
              src_account: src,
              dst_account: dest,
              dst_amount: {
                value: String(Math.min(0.1, potentialBalanceAdj)),
                issuer: dest,
                currency: line.currency
              }
            };

            outstanding++;
            var complete = false;

            function request() {
              // console.log({msg: 'request', options});

              if (CLOSE_FIRST) {
                remote.requestPathFindClose(() => {
                  console.log({msg: 'close_first', remote: remote.remoteIndex});
                });
              }

              var pf = remote.path_find(options);
              pf.on('error', (e) => {
                // done(new Error(prettyJSON(e)));
                requests.push(req);
                outstanding--;
                pf[DISPOSE_OR_CLOSE]()
                doOne(remote);
              });
              pf.on('update', (m) => {
                if (WAIT_FULL_REPLY && !m.full_reply) {
                  return;
                }
                complete = true;
                finished++;
                outstanding--;
                console.log(prettyJSON({msg: 'update', update: m}));
                console.log({msg: 'finished', finished,
                             remote: remote.remoteIndex, t: new Date()});
                pf[DISPOSE_OR_CLOSE]()
                doOne(remote);
              });
              if (RETRY_AFTER !== 0) {
                setTimeout(() => {
                  if (!complete) {
                    console.log({msg: 'retrying', remote: remote.remoteIndex});
                    pf[DISPOSE_OR_CLOSE]();
                    request();
                  }
                }, RETRY_AFTER);
              }
            }
            request();
          }
          remotes.forEach((remote, ix) => {
            remote.remoteIndex = ix;
            doOne(remote);
          });
          // doOne(remote);
        });
      });
    }
  }
);
